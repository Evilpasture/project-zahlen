// src/engine/TTYBackend.cpp
#include "TTYBackend.hpp"

#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <print>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h> // For major() and minor()
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ZHLN::TTYBackend {

#ifdef __linux__

struct TakenDevice {
	uint32_t maj;
	uint32_t min;
	int fd;
	libevdev* dev;
};

struct TTYState {
	int tty_fd = -1;
	int old_kb_mode = 0;
	struct termios old_termios;
	uint32_t width = 0;
	uint32_t height = 0;
	bool running = true;

	int epoll_fd = -1;
	std::vector<TakenDevice> taken_devices;

	// systemd-logind session state
	sd_bus* bus = nullptr;
	char* session_id = nullptr;
	std::string session_path;
	bool has_logind_control = false;
};

// Global pointer specifically for the crash handler to access
static TTYState* g_CrashState = nullptr;

static KeyCode MapEvdevKey(uint16_t code) {
	switch (code) {
		case KEY_W:
			return KeyCode::W;
		case KEY_A:
			return KeyCode::A;
		case KEY_S:
			return KeyCode::S;
		case KEY_D:
			return KeyCode::D;
		case KEY_LEFTSHIFT:
			return KeyCode::LShift;
		case KEY_SPACE:
			return KeyCode::Space;
		case KEY_ESC:
			return KeyCode::Escape;
		case KEY_R:
			return KeyCode::R;
		case BTN_LEFT:
			return KeyCode::RButton; // Map standard clicks
		case BTN_RIGHT:
			return KeyCode::RButton;
		default:
			return KeyCode::Unknown;
	}
}

bool IsSupported() {
	return access("/dev/tty", R_OK | W_OK) == 0;
}

void* Init(uint32_t width, uint32_t height) {
	auto* state = new TTYState();
	state->width = width;
	state->height = height;

	state->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (state->tty_fd >= 0) {
		ioctl(state->tty_fd, KDGKBMODE, &state->old_kb_mode);

		struct termios t{};
		tcgetattr(state->tty_fd, &t);
		state->old_termios = t;

		t.c_lflag &= ~(ICANON | ECHO);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;

		tcsetattr(state->tty_fd, TCSANOW, &t);
		ioctl(state->tty_fd, KDSETMODE, KD_GRAPHICS);
	}

	// 1. Try to acquire systemd-logind Session Control
	if (sd_pid_get_session(0, &state->session_id) >= 0) {
		if (sd_bus_open_system(&state->bus) >= 0) {
			state->session_path =
				std::string("/org/freedesktop/login1/session/") + state->session_id;
			sd_bus_error error = SD_BUS_ERROR_NULL;

			int r =
				sd_bus_call_method(state->bus, "org.freedesktop.login1",
								   state->session_path.c_str(), "org.freedesktop.login1.Session",
								   "TakeControl", &error, nullptr, "b", 0); // force = false
			if (r >= 0) {
				state->has_logind_control = true;
				ZHLN::Log("[TTY] Successfully acquired systemd-logind Session Control.");
			} else {
				ZHLN::Log(
					"[TTY] Logind TakeControl failed: {} (errno: {}). Falling back to direct open.",
					(error.message != nullptr) ? error.message : "unknown", r);
				sd_bus_error_free(&error);
			}
		}
	}

	// 2. Initialize epoll
	state->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (state->epoll_fd >= 0) {
		DIR* dir = opendir("/dev/input");
		if (dir != nullptr) {
			struct dirent* ent = nullptr;
			while ((ent = readdir(dir)) != nullptr) {
				if (strncmp(ent->d_name, "event", 5) == 0) {
					std::string path = std::string("/dev/input/") + ent->d_name;

					int fd = -1;
					bool is_logind_fd = false;
					struct stat st{};

					if (stat(path.c_str(), &st) == 0) {
						uint32_t maj = major(st.st_rdev);
						uint32_t min = minor(st.st_rdev);

						if (state->has_logind_control) {
							sd_bus_error error = SD_BUS_ERROR_NULL;
							sd_bus_message* reply = nullptr;

							int r = sd_bus_call_method(
								state->bus, "org.freedesktop.login1", state->session_path.c_str(),
								"org.freedesktop.login1.Session", "TakeDevice", &error, &reply,
								"uu", maj, min);
							if (r >= 0) {
								int paused = 0;
								sd_bus_message_read(reply, "hb", &fd, &paused);
								is_logind_fd = true;

								if (paused != 0) {
									ZHLN::Log("[TTY] WARNING: systemd-logind paused device '{}' on "
											  "startup!",
											  path);
								}

								int flags = fcntl(fd, F_GETFL, 0);
								fcntl(fd, F_SETFL, flags | O_NONBLOCK);
							} else {
								ZHLN::Log(
									"[TTY] systemd-logind rejected device '{}': {} (error: {})",
									path, (error.message != nullptr) ? error.message : "unknown",
									r);
								sd_bus_error_free(&error);
							}
							if (reply != nullptr) {
								sd_bus_message_unref(reply);
							}
						}
					}

					// Fallback to standard open if logind failed or was bypassed
					if (fd < 0) {
						fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
					}

					if (fd >= 0) {
						libevdev* dev = nullptr;
						if (libevdev_new_from_fd(fd, &dev) == 0) {
							if (libevdev_has_event_type(dev, EV_KEY) ||
								libevdev_has_event_type(dev, EV_REL)) {

								struct stat dev_st{};
								stat(path.c_str(), &dev_st);
								state->taken_devices.push_back({.maj = major(dev_st.st_rdev),
																.min = minor(dev_st.st_rdev),
																.fd = fd,
																.dev = dev});

								epoll_event ep_ev{};
								ep_ev.events = EPOLLIN;
								ep_ev.data.ptr = dev;
								epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ep_ev);

								ZHLN::Log("[TTY] Monitoring input device: {} ({})",
										  libevdev_get_name(dev),
										  is_logind_fd ? "via systemd-logind" : "via direct open");
							} else {
								libevdev_free(dev);
								close(fd);
							}
						}
					}
				}
			}
			closedir(dir);
		}
	}

	g_CrashState = state;
	return state;
}

void Shutdown(void* context) {
	auto* state = static_cast<TTYState*>(context);
	if (state != nullptr) {
		if (state->tty_fd >= 0) {
			ioctl(state->tty_fd, KDSETMODE, KD_TEXT);
			tcsetattr(state->tty_fd, TCSANOW, &state->old_termios);
			tcflush(state->tty_fd, TCIFLUSH);
			close(state->tty_fd);
		}

		// Clean up taken devices
		for (const auto& td : state->taken_devices) {
			libevdev_free(td.dev);

			if (state->has_logind_control) {
				sd_bus_error error = SD_BUS_ERROR_NULL;
				sd_bus_call_method(state->bus, "org.freedesktop.login1",
								   state->session_path.c_str(), "org.freedesktop.login1.Session",
								   "ReleaseDevice", &error, nullptr, "uu", td.maj, td.min);
				sd_bus_error_free(&error);
			}
			close(td.fd);
		}

		if (state->has_logind_control) {
			sd_bus_error error = SD_BUS_ERROR_NULL;
			sd_bus_call_method(state->bus, "org.freedesktop.login1", state->session_path.c_str(),
							   "org.freedesktop.login1.Session", "ReleaseControl", &error, nullptr,
							   "");
			sd_bus_error_free(&error);
		}

		if (state->bus != nullptr) {
			sd_bus_unref(state->bus);
		}
		if (state->session_id != nullptr) {
			free(state->session_id);
		}

		if (state->epoll_fd >= 0) {
			close(state->epoll_fd);
		}

		g_CrashState = nullptr;
		delete state;
	}
}

bool IsRunning(void* context) {
	auto* state = static_cast<TTYState*>(context);
	return (state != nullptr) ? state->running : false;
}

void EmergencyRestore() {
	if ((g_CrashState != nullptr) && g_CrashState->tty_fd >= 0) {
		ioctl(g_CrashState->tty_fd, KDSETMODE, KD_TEXT);
		ioctl(g_CrashState->tty_fd, KDSKBMODE, K_XLATE);
		tcsetattr(g_CrashState->tty_fd, TCSANOW, &g_CrashState->old_termios);
		tcflush(g_CrashState->tty_fd, TCIFLUSH);

		// Safely notify logind we've dropped session control
		if (g_CrashState->has_logind_control) {
			sd_bus_error error = SD_BUS_ERROR_NULL;
			sd_bus_call_method(g_CrashState->bus, "org.freedesktop.login1",
							   g_CrashState->session_path.c_str(), "org.freedesktop.login1.Session",
							   "ReleaseControl", &error, nullptr, "");
			sd_bus_error_free(&error);
		}
	}
}

void ProcessEvents(void* context, InputContext* input) {
	auto* state = static_cast<TTYState*>(context);
	if ((state == nullptr) || state->epoll_fd < 0 || (input == nullptr)) {
		return;
	}

	epoll_event events[16];
	int n = epoll_wait(state->epoll_fd, events, 16, 0);

	float mouseAccumX = 0.0f;
	float mouseAccumY = 0.0f;
	bool mouseMoved = false;

	for (int i = 0; i < n; i++) {
		auto* dev = static_cast<libevdev*>(events[i].data.ptr);
		input_event ev{};
		int rc = 0;

		// --- SOLIDIFIED NON-BLOCKING EVENT LOOP ---
		while (true) {
			rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

			if (rc == -EAGAIN) {
				break; // Queue is empty, exit loop cleanly
			}

			if (rc == LIBEVDEV_READ_STATUS_SYNC) {
				// Buffer out-of-sync; safely drain the sync queue using FLAG_SYNC to prevent
				// infinite loop
				while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) ==
					   LIBEVDEV_READ_STATUS_SYNC) {
					// Consuming sync events silently
				}
				break; // Exit this frame's read pass immediately to let the buffer recover
			}

			if (rc < 0) {
				break; // Other error, exit loop
			}

			// 1. Keys & Buttons
			if (ev.type == EV_KEY) {
				KeyCode key = MapEvdevKey(ev.code);
				if (key != KeyCode::Unknown) {
					if (ev.value == 1 || ev.value == 2) {
						input->InjectKeyDown(key);
					} else if (ev.value == 0) {
						input->InjectKeyUp(key);
					}
				}

				if (ev.code == KEY_ESC && ev.value == 1) {
					state->running = false;
				}
			}

			// 2. Mouse Axes
			if (ev.type == EV_REL) {
				if (ev.code == REL_X) {
					mouseAccumX += (float)ev.value;
					mouseMoved = true;
				} else if (ev.code == REL_Y) {
					mouseAccumY += (float)ev.value;
					mouseMoved = true;
				}

				if (ev.code == REL_WHEEL) {
					input->InjectWheelMotion((float)ev.value);
				}
			}
		}
	}

	if (mouseMoved) {
		static float virtualMouseX = 960.0f;
		static float virtualMouseY = 540.0f;

		virtualMouseX += mouseAccumX;
		virtualMouseY += mouseAccumY;

		input->InjectLocalMotion(virtualMouseX, virtualMouseY);
	}
}

std::vector<const char*> GetRequiredInstanceExtensions() {
	return {
		VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME,
		// Require these on the instance level to satisfy swapchain_maintenance1 device requirements
		VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
		VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
}

VkSurfaceKHR CreateSurface(VkInstance instance, VkPhysicalDevice physical, void* context,
						   uint32_t& outWidth, uint32_t& outHeight) {
	uint32_t displayCount = 0;
	vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &displayCount, nullptr);
	if (displayCount == 0) {
		ZHLN::Log("[TTY] FATAL: No displays found via VK_KHR_display");
		return VK_NULL_HANDLE;
	}

	std::vector<VkDisplayPropertiesKHR> displays(displayCount);
	vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &displayCount, displays.data());
	VkDisplayKHR targetDisplay = displays[0].display;

	ZHLN::Log("[TTY] Using Display: {} (Resolution: {}x{})",
			  (displays[0].displayName != nullptr) ? displays[0].displayName : "Unknown",
			  displays[0].physicalResolution.width, displays[0].physicalResolution.height);

	uint32_t modeCount = 0;
	vkGetDisplayModePropertiesKHR(physical, targetDisplay, &modeCount, nullptr);
	std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
	vkGetDisplayModePropertiesKHR(physical, targetDisplay, &modeCount, modes.data());

	VkDisplayModeKHR targetMode = modes[0].displayMode;
	outWidth = modes[0].parameters.visibleRegion.width;
	outHeight = modes[0].parameters.visibleRegion.height;

	ZHLN::Log("[TTY] Selected Mode: {}x{} @ {}Hz", outWidth, outHeight,
			  (float)modes[0].parameters.refreshRate / 1000.0f);

	// --- FIX: Retrieve Plane Properties FIRST to satisfy validation requirements ---
	uint32_t planeCount = 0;
	vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &planeCount, nullptr);
	std::vector<VkDisplayPlanePropertiesKHR> planeProps(planeCount);
	vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &planeCount, planeProps.data());

	uint32_t targetPlane = UINT32_MAX;
	for (uint32_t i = 0; i < planeCount; i++) {
		if (planeProps[i].currentDisplay != VK_NULL_HANDLE &&
			planeProps[i].currentDisplay != targetDisplay) {
			continue;
		}

		uint32_t supportedCount = 0;
		vkGetDisplayPlaneSupportedDisplaysKHR(physical, i, &supportedCount, nullptr);
		std::vector<VkDisplayKHR> supported(supportedCount);
		vkGetDisplayPlaneSupportedDisplaysKHR(physical, i, &supportedCount, supported.data());

		for (auto* d : supported) {
			if (d == targetDisplay) {
				targetPlane = i;
				break;
			}
		}
		if (targetPlane != UINT32_MAX) {
			break;
		}
	}

	if (targetPlane == UINT32_MAX) {
		ZHLN::Log("[TTY] FATAL: Could not find a compatible display plane!");
		return VK_NULL_HANDLE;
	}

	VkDisplayPlaneCapabilitiesKHR planeCaps;
	vkGetDisplayPlaneCapabilitiesKHR(physical, targetMode, targetPlane, &planeCaps);

	VkDisplayPlaneAlphaFlagBitsKHR alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	if (!(planeCaps.supportedAlpha & alphaMode)) {
		if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR) {
			alphaMode = VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;
		} else if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR) {
			alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR;
		} else if (planeCaps.supportedAlpha &
				   VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR) {
			alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR;
		}
	}

	VkDisplaySurfaceCreateInfoKHR createInfo = {
		.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
		.pNext = nullptr,
		.flags = 0,
		.displayMode = targetMode,
		.planeIndex = targetPlane,
		.planeStackIndex = 0,
		.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.globalAlpha = 1.0f,
		.alphaMode = alphaMode,
		.imageExtent = {.width = outWidth, .height = outHeight}};

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	if (vkCreateDisplayPlaneSurfaceKHR(instance, &createInfo, nullptr, &surface) != VK_SUCCESS) {
		ZHLN::Log("[TTY] FATAL: vkCreateDisplayPlaneSurfaceKHR failed!");
	} else {
		ZHLN::Log("[TTY] VK_KHR_display Surface successfully created on Plane {}", targetPlane);
	}

	return surface;
}

#else

// --- FALLBACK STUBS FOR NON-LINUX BUILDS (Windows/macOS) ---
bool IsSupported() {
	return false;
}
void* Init(uint32_t, uint32_t) {
	return nullptr;
}
void Shutdown(void*) {}
void EmergencyRestore() {}
void ProcessEvents(void*, InputContext*) {}
std::vector<const char*> GetRequiredInstanceExtensions() {
	return {};
}
VkSurfaceKHR CreateSurface(VkInstance, VkPhysicalDevice, void*, uint32_t&, uint32_t&) {
	return VK_NULL_HANDLE;
}

#endif

} // namespace ZHLN::TTYBackend
