// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/TTYBackend.cpp
#include "TTYBackend.hpp"

#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
extern "C" {
#include <libseat.h>
}
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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
	int device_id; // Assigned by libseat
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

	struct libseat* seat = nullptr;
	bool active = false;
};

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
			return KeyCode::RButton;
		case BTN_RIGHT:
			return KeyCode::RButton;
		default:
			return KeyCode::Unknown;
	}
}

static void handle_enable_seat(struct libseat* seat, void* data) {
	auto* state = static_cast<TTYState*>(data);
	state->active = true;
	ZHLN::Log("[TTY] libseat: Seat session enabled and active.");
}

static void handle_disable_seat(struct libseat* seat, void* data) {
	auto* state = static_cast<TTYState*>(data);
	state->active = false;
	ZHLN::Log("[TTY] libseat: Seat session disabled (VT switched away).");
	libseat_disable_seat(seat);
}

static struct libseat_seat_listener seat_listener = {
	.enable_seat = handle_enable_seat,
	.disable_seat = handle_disable_seat,
};

bool IsSupported() {
	return access("/dev/tty", R_OK | W_OK) == 0;
}

void* Init(uint32_t width, uint32_t height) {
	auto* state = new TTYState();
	state->width = width;
	state->height = height;

	state->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (state->tty_fd >= 0) {
		if (ioctl(state->tty_fd, KDGKBMODE, &state->old_kb_mode) >= 0) {
			struct termios t{};
			tcgetattr(state->tty_fd, &t);
			state->old_termios = t;

			t.c_lflag &= ~(ICANON | ECHO);
			t.c_cc[VMIN] = 0;
			t.c_cc[VTIME] = 0;

			tcsetattr(state->tty_fd, TCSANOW, &t);
			ioctl(state->tty_fd, KDSETMODE, KD_GRAPHICS);
			ioctl(state->tty_fd, KDSKBMODE, K_MEDIUMRAW);
			ZHLN::Log("[TTY] Virtual Console (VT) initialized.");
		} else {
			close(state->tty_fd);
			state->tty_fd = -1;
		}
	}

	// 1. Establish libseat session
	libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);
	state->seat = libseat_open_seat(&seat_listener, state);
	if (state->seat == nullptr) {
		ZHLN::Log("[TTY] FATAL: Failed to initialize libseat session.");
		Shutdown(state);
		return nullptr;
	}

	// Dispatch initial setup events until seat is marked active
	while (!state->active) {
		if (libseat_dispatch(state->seat, -1) == -1) {
			ZHLN::Log("[TTY] FATAL: Error dispatching libseat during startup.");
			Shutdown(state);
			return nullptr;
		}
	}

	// 2. Initialize epoll
	state->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (state->epoll_fd < 0) {
		Shutdown(state);
		return nullptr;
	}

	// Add the libseat connection FD to epoll so we get notified on session switches
	int seat_fd = libseat_get_fd(state->seat);
	if (seat_fd >= 0) {
		epoll_event ev{};
		ev.events = EPOLLIN;
		ev.data.ptr = state->seat; // Store pointer to differentiate from evdev
		epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, seat_fd, &ev);
	}

	// 3. Scan and Open input devices via libseat
	DIR* dir = opendir("/dev/input");
	if (dir != nullptr) {
		struct dirent* ent = nullptr;
		while ((ent = readdir(dir)) != nullptr) {
			if (strncmp(ent->d_name, "event", 5) == 0) {
				std::string path = std::string("/dev/input/") + ent->d_name;

				int fd = -1;
				int device_id = libseat_open_device(state->seat, path.c_str(), &fd);

				if (device_id >= 0 && fd >= 0) {
					libevdev* dev = nullptr;
					if (libevdev_new_from_fd(fd, &dev) == 0) {
						if (libevdev_has_event_type(dev, EV_KEY) ||
							libevdev_has_event_type(dev, EV_REL)) {

							libevdev_grab(dev, LIBEVDEV_GRAB);

							struct stat dev_st{};
							stat(path.c_str(), &dev_st);

							state->taken_devices.push_back({.maj = major(dev_st.st_rdev),
															.min = minor(dev_st.st_rdev),
															.fd = fd,
															.dev = dev,
															.device_id = device_id});

							epoll_event ep_ev{};
							ep_ev.events = EPOLLIN;
							ep_ev.data.ptr = dev;
							epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ep_ev);

							ZHLN::Log("[TTY] libseat opened input device: {} (ID: {})",
									  libevdev_get_name(dev), device_id);
						} else {
							libevdev_free(dev);
							libseat_close_device(state->seat, device_id);
						}
					} else {
						libseat_close_device(state->seat, device_id);
					}
				}
			}
		}
		closedir(dir);
	}

	if (state->taken_devices.empty()) {
		ZHLN::Log("[TTY] FATAL: No input devices could be opened under this seat.");
		Shutdown(state);
		return nullptr;
	}

	g_CrashState = state;
	return state;
}

void Shutdown(void* context) {
	auto* state = static_cast<TTYState*>(context);
	if (state != nullptr) {
		if (state->tty_fd >= 0) {
			ioctl(state->tty_fd, KDSETMODE, KD_TEXT);
			ioctl(state->tty_fd, KDSKBMODE, state->old_kb_mode);
			tcsetattr(state->tty_fd, TCSANOW, &state->old_termios);
			tcflush(state->tty_fd, TCIFLUSH);
			close(state->tty_fd);
		}

		for (const auto& td : state->taken_devices) {
			if (td.dev != nullptr) {
				libevdev_grab(td.dev, LIBEVDEV_UNGRAB);
				libevdev_free(td.dev);
			}
			if (state->seat != nullptr && td.device_id >= 0) {
				libseat_close_device(state->seat, td.device_id);
			}
		}

		if (state->seat != nullptr) {
			libseat_close_seat(state->seat);
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
		ioctl(g_CrashState->tty_fd, KDSKBMODE, g_CrashState->old_kb_mode);
		tcsetattr(g_CrashState->tty_fd, TCSANOW, &g_CrashState->old_termios);
		tcflush(g_CrashState->tty_fd, TCIFLUSH);

		for (const auto& td : g_CrashState->taken_devices) {
			if (td.dev != nullptr) {
				libevdev_grab(td.dev, LIBEVDEV_UNGRAB);
			}
			if (g_CrashState->seat != nullptr && td.device_id >= 0) {
				libseat_close_device(g_CrashState->seat, td.device_id);
			}
		}

		if (g_CrashState->seat != nullptr) {
			libseat_close_seat(g_CrashState->seat);
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

	static bool ctrlDown = false;
	static bool altDown = false;

	for (int i = 0; i < n; i++) {
		// --- Process internal libseat messages ---
		if (events[i].data.ptr == state->seat) {
			libseat_dispatch(state->seat, 0);
			continue;
		}

		auto* dev = static_cast<libevdev*>(events[i].data.ptr);
		input_event ev{};
		int rc = 0;

		while (true) {
			rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

			if (rc == -EAGAIN) {
				break;
			}

			if (rc == LIBEVDEV_READ_STATUS_SYNC) {
				while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) ==
					   LIBEVDEV_READ_STATUS_SYNC) {
				}
				break;
			}

			if (rc < 0) {
				break;
			}

			if (ev.type == EV_KEY) {
				if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
					ctrlDown = (ev.value != 0);
				}
				if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
					altDown = (ev.value != 0);
				}

				// --- EMERGENCY ESCAPE HATCH ---
				if (ctrlDown && altDown && ev.code == KEY_BACKSPACE && ev.value == 1) {
					ZHLN::Log("[TTY] Emergency Escape Hatch triggered! Restoring terminal...");
					EmergencyRestore();
					_exit(0);
				}

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
	return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME,
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
