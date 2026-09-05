// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/TTYBackend.cpp
#include "TTYBackend.hpp"
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window.hpp>

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
namespace {
struct TakenDevice {
    uint32_t  maj;
    uint32_t  min;
    int       fd;
    libevdev* dev;
    int       device_id; // Assigned by libseat
};

struct TTYState {
    int            tty_fd      = -1;
    int            old_kb_mode = 0;
    struct termios old_termios {};
    uint32_t       width   = 0;
    uint32_t       height  = 0;
    bool           running = true;

    int                      epoll_fd = -1;
    std::vector<TakenDevice> taken_devices;

    struct libseat* seat   = nullptr;
    bool            active = false;
};

TTYState* g_CrashState = nullptr;

// 1. Consteval mapping (Natural forward direction)
[[maybe_unused]] consteval auto KeyCodeToEvdev(KeyCode key) noexcept -> uint16_t {
    using enum KeyCode;
    switch (key) {
        // Numbers 0 - 9
        case Num0:
            return KEY_0;
        case Num1:
            return KEY_1;
        case Num2:
            return KEY_2;
        case Num3:
            return KEY_3;
        case Num4:
            return KEY_4;
        case Num5:
            return KEY_5;
        case Num6:
            return KEY_6;
        case Num7:
            return KEY_7;
        case Num8:
            return KEY_8;
        case Num9:
            return KEY_9;

        // Alphabet A - Z
        case A:
            return KEY_A;
        case B:
            return KEY_B;
        case C:
            return KEY_C;
        case D:
            return KEY_D;
        case E:
            return KEY_E;
        case F:
            return KEY_F;
        case G:
            return KEY_G;
        case H:
            return KEY_H;
        case I:
            return KEY_I;
        case J:
            return KEY_J;
        case K:
            return KEY_K;
        case L:
            return KEY_L;
        case M:
            return KEY_M;
        case N:
            return KEY_N;
        case O:
            return KEY_O;
        case P:
            return KEY_P;
        case Q:
            return KEY_Q;
        case R:
            return KEY_R;
        case S:
            return KEY_S;
        case T:
            return KEY_T;
        case U:
            return KEY_U;
        case V:
            return KEY_V;
        case W:
            return KEY_W;
        case X:
            return KEY_X;
        case Y:
            return KEY_Y;
        case Z:
            return KEY_Z;

        // Function Keys F1 - F12
        case F1:
            return KEY_F1;
        case F2:
            return KEY_F2;
        case F3:
            return KEY_F3;
        case F4:
            return KEY_F4;
        case F5:
            return KEY_F5;
        case F6:
            return KEY_F6;
        case F7:
            return KEY_F7;
        case F8:
            return KEY_F8;
        case F9:
            return KEY_F9;
        case F10:
            return KEY_F10;
        case F11:
            return KEY_F11;
        case F12:
            return KEY_F12;

        // Modifiers
        case LShift:
            return KEY_LEFTSHIFT;
        case RShift:
            return KEY_RIGHTSHIFT;
        case LControl:
            return KEY_LEFTCTRL;
        case RControl:
            return KEY_RIGHTCTRL;
        case LAlt:
            return KEY_LEFTALT;
        case RAlt:
            return KEY_RIGHTALT;

        // Navigation & Editing
        case Space:
            return KEY_SPACE;
        case Escape:
            return KEY_ESC;
        case Enter:
            return KEY_ENTER;
        case Backspace:
            return KEY_BACKSPACE;
        case Tab:
            return KEY_TAB;
        case Delete:
            return KEY_DELETE;

        // Arrow Keys
        case Up:
            return KEY_UP;
        case Down:
            return KEY_DOWN;
        case Left:
            return KEY_LEFT;
        case Right:
            return KEY_RIGHT;

        // Line / page navigation
        case Home:
            return KEY_HOME;
        case End:
            return KEY_END;
        case PageUp:
            return KEY_PAGEUP;
        case PageDown:
            return KEY_PAGEDOWN;

        // Mouse Buttons
        case LButton:
            return BTN_LEFT;
        case RButton:
            return BTN_RIGHT;
        case MButton:
            return BTN_MIDDLE;

        default:
            return 0;
    }
}

// 2. C++26 Reflection generates the inverted O(1) lookup table at compile time!
consteval auto BuildEvdevToKeyCodeTable() noexcept {
    std::array<KeyCode, KEY_MAX + 1> table {};
    table.fill(KeyCode::Unknown);

    ZHLN::Reflect::ForEachEnumerator<KeyCode>([&]<KeyCode Key>() -> auto {
        constexpr uint16_t evdevCode = KeyCodeToEvdev(Key);
        if constexpr (evdevCode > 0 && evdevCode <= KEY_MAX) {
            table[evdevCode] = Key;
        }
    });

    return table;
}

// 3. Runtime function: Single instruction array access (O(1) / Branchless)
auto MapEvdevKey(uint16_t code) noexcept -> KeyCode {
    static constexpr auto Table = BuildEvdevToKeyCodeTable();
    if (code <= KEY_MAX) [[likely]] {
        return Table[code];
    }
    return KeyCode::Unknown;
}

void handle_enable_seat(struct libseat* /*seat*/, void* data) {
    auto* state   = static_cast<TTYState*>(data);
    state->active = true;
    ZHLN::Log("[TTY] libseat: Seat session enabled and active.");
}

void handle_disable_seat(struct libseat* seat, void* data) {
    auto* state   = static_cast<TTYState*>(data);
    state->active = false;
    ZHLN::Log("[TTY] libseat: Seat session disabled (VT switched away).");
    libseat_disable_seat(seat);
}

struct libseat_seat_listener seat_listener = {
    .enable_seat  = handle_enable_seat,
    .disable_seat = handle_disable_seat,
};
} // namespace

auto IsSupported() -> bool {
    return access("/dev/tty", R_OK | W_OK) == 0;
}

auto Init(uint32_t width, uint32_t height) -> void* {
    auto* state   = new TTYState();
    state->width  = width;
    state->height = height;

    state->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (state->tty_fd >= 0) {
        if (ioctl(state->tty_fd, KDGKBMODE, &state->old_kb_mode) >= 0) {
            struct termios t {};
            tcgetattr(state->tty_fd, &t);
            state->old_termios = t;

            t.c_lflag &= ~(ICANON | ECHO);
            t.c_cc[VMIN]  = 0;
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
        epoll_event ev {};
        ev.events   = EPOLLIN;
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

                int fd        = -1;
                int device_id = libseat_open_device(state->seat, path.c_str(), &fd);

                if (device_id >= 0 && fd >= 0) {
                    libevdev* dev = nullptr;
                    if (libevdev_new_from_fd(fd, &dev) == 0) {
                        if (libevdev_has_event_type(dev, EV_KEY) || libevdev_has_event_type(dev, EV_REL)) {
                            libevdev_grab(dev, LIBEVDEV_GRAB);

                            struct stat dev_st {};
                            stat(path.c_str(), &dev_st);

                            state->taken_devices.push_back(
                                {.maj = major(dev_st.st_rdev), .min = minor(dev_st.st_rdev), .fd = fd, .dev = dev, .device_id = device_id}
                            );

                            epoll_event ep_ev {};
                            ep_ev.events   = EPOLLIN;
                            ep_ev.data.ptr = dev;
                            epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ep_ev);

                            ZHLN::Log("[TTY] libseat opened input device: {} (ID: {})", libevdev_get_name(dev), device_id);
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

        for (const auto& td: state->taken_devices) {
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

auto IsRunning(void* context) -> bool {
    auto* state = static_cast<TTYState*>(context);
    return (state != nullptr) ? state->running : false;
}

void EmergencyRestore() {
    if ((g_CrashState != nullptr) && g_CrashState->tty_fd >= 0) {
        ioctl(g_CrashState->tty_fd, KDSETMODE, KD_TEXT);
        ioctl(g_CrashState->tty_fd, KDSKBMODE, g_CrashState->old_kb_mode);
        tcsetattr(g_CrashState->tty_fd, TCSANOW, &g_CrashState->old_termios);
        tcflush(g_CrashState->tty_fd, TCIFLUSH);

        for (const auto& td: g_CrashState->taken_devices) {
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

void ProcessEvents(void* context, const WindowInputReceiver& receiver) {
    auto* state = static_cast<TTYState*>(context);
    if ((state == nullptr) || state->epoll_fd < 0) {
        return;
    }

    std::array<epoll_event, 16> events {};
    int                         n = epoll_wait(state->epoll_fd, events.data(), 16, 0);

    float mouseAccumX = 0.0f;
    float mouseAccumY = 0.0f;
    bool  mouseMoved  = false;

    static bool ctrlDown = false;
    static bool altDown  = false;

    for (int i = 0; i < n; i++) {
        // --- Process internal libseat messages ---
        if (events[i].data.ptr == state->seat) {
            libseat_dispatch(state->seat, 0);
            continue;
        }

        auto*       dev = static_cast<libevdev*>(events[i].data.ptr);
        input_event ev {};
        int         rc = 0;

        while (true) {
            rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

            if (rc == -EAGAIN) {
                break;
            }

            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC) {
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
                if (key != KeyCode::Unknown && (receiver.onKey != nullptr)) {
                    if (ev.value == 1 || ev.value == 2) {
                        receiver.onKey(receiver.userdata, key, true);
                    } else if (ev.value == 0) {
                        receiver.onKey(receiver.userdata, key, false);
                    }
                }

                if (ev.code == KEY_ESC && ev.value == 1) {
                    state->running = false;
                }
            }

            if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    mouseAccumX += static_cast<float>(ev.value);
                    mouseMoved = true;
                } else if (ev.code == REL_Y) {
                    mouseAccumY += static_cast<float>(ev.value);
                    mouseMoved = true;
                }

                if (ev.code == REL_WHEEL && (receiver.onMouseScroll != nullptr)) {
                    receiver.onMouseScroll(receiver.userdata, static_cast<float>(ev.value));
                }
            }
        }
    }

    if (mouseMoved && (receiver.onMouseMove != nullptr)) {
        static float virtualMouseX = 960.0f;
        static float virtualMouseY = 540.0f;

        virtualMouseX += mouseAccumX;
        virtualMouseY += mouseAccumY;

        receiver.onMouseMove(receiver.userdata, virtualMouseX, virtualMouseY);
    }
}

auto GetRequiredInstanceExtensions() -> std::vector<std::string_view> {
    return {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
    };
}

#else

bool IsSupported() {
    return false;
}
void* Init(uint32_t, uint32_t) {
    return nullptr;
}
void Shutdown(void*) {
}
void EmergencyRestore() {
}
void ProcessEvents(void*, const WindowInputReceiver&) {
}
std::vector<std::string_view> GetRequiredInstanceExtensions() {
    return {};
}

bool IsRunning(void*) {
    return false;
}

#endif

} // namespace ZHLN::TTYBackend
