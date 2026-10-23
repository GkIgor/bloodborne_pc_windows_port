// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the host side of bbhost's menu pointer (bbhost/menu_pointer.cpp) - the window's mouse,
// the pad, the settings and the presses the pointer makes. See licenses/BBHOST-PORTS.md.

#include "bbport_menu_pointer.h"
#include "bbhost/compat.h"
#include "bbhost/menu_pointer.h"
#include "bbport_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace {

// The list vtable's update slot, to confirm menu_pointer_install put the hook in.
constexpr std::uint64_t kListVTable = 0x5799fb0, kListUpdate = 0x1ecff30;
constexpr std::size_t kListUpdateSlot = 3;
constexpr std::uint32_t kPadUp = 0x10, kPadDown = 0x40;

// Item Storage. The flow's open menu (+0xa88 of the flow at 0x5980e38) is a sequence (update
// 0x201bc10 in vtable slot 3) running step [+0xc] of the steps at +0x10; storage's step holds at
// +0x10 the named step (vtable 0x5745ee0) that sub_1fba4f0 makes with the name L"depository" at
// +0x30. Its item list's listener has vtable 0x57393b0 (the inventory's too, by the same code).
constexpr std::uint64_t kMenuFlow = 0x5980e38, kSequenceUpdate = 0x201bc10, kNamedStep = 0x5745ee0;
constexpr std::uint64_t kDepositoryName = 0x4db4d84, kItemListListener = 0x57393b0;

bool g_installed = false;
std::uint64_t g_image_base = 0;
std::atomic<std::int64_t> g_storage_ms{0};

std::atomic<float> g_mouse_x{0.0f}, g_mouse_y{0.0f};
std::atomic<int> g_window_w{0}, g_window_h{0};
std::atomic<bool> g_focused{true}, g_inside{true}, g_relative{false}, g_pad_device{false};

std::atomic<std::uint32_t> g_pad_buttons{0};
std::atomic<std::uint8_t> g_pad_lx{128}, g_pad_ly{128};

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Presses the pad sees for a few samples each, one after another (clicks, wheel notches).
constexpr std::int64_t kPulseHoldMs = 50, kPulseGapMs = 50;
std::mutex g_pulse_lock;
std::deque<std::uint32_t> g_pulses;
std::uint32_t g_pulse_held = 0;
std::int64_t g_pulse_until = 0;

void push_pulse(std::uint32_t buttons) {
    std::scoped_lock lock{g_pulse_lock};
    if (g_pulses.size() < 8) {
        g_pulses.push_back(buttons);
    }
}

std::uint32_t take_pulse() {
    const std::int64_t now = now_ms();
    std::scoped_lock lock{g_pulse_lock};
    if (g_pulse_held && now < g_pulse_until) {
        return g_pulse_held;
    }
    if (g_pulse_held) {
        g_pulse_held = 0;
        g_pulse_until = now + kPulseGapMs;
        return 0;
    }
    if (now < g_pulse_until || g_pulses.empty()) {
        return 0;
    }
    g_pulse_held = g_pulses.front();
    g_pulses.pop_front();
    g_pulse_until = now + kPulseHoldMs;
    return g_pulse_held;
}

void clear_pulses() {
    std::scoped_lock lock{g_pulse_lock};
    g_pulses.clear();
    g_pulse_held = 0;
}

std::uint64_t guest_at(std::uint64_t bn) {
    return g_image_base + (bn - kPreferredGuestSlide);
}

std::uint64_t read_u64(std::uint64_t addr) {
    std::uint64_t v = 0;
    return addr >= 0x1000 && host_read_safe(reinterpret_cast<const void*>(addr), &v, sizeof(v)) ? v : 0;
}

// The name of the named step the open menu is running (a UTF-16 string in the eboot), or 0.
std::uint64_t open_menu_step_name() {
    const std::uint64_t flow = read_u64(guest_at(kMenuFlow));
    std::uint64_t step = flow ? read_u64(flow + 0xa88) : 0;
    for (int depth = 0; step && depth < 8; ++depth) {
        const std::uint64_t vt = read_u64(step);
        if (vt == guest_at(kNamedStep)) {
            return read_u64(step + 0x30);
        }
        if (vt && read_u64(vt + 3 * 8) == guest_at(kSequenceUpdate)) {
            const std::uint64_t index = read_u64(step + 0x8) >> 32, count = read_u64(step + 0x58);
            step = index < count ? read_u64(step + 0x10 + index * 8) : 0;
        } else {
            step = read_u64(step + 0x10);
        }
    }
    return 0;
}

} // namespace

// Game thread, every list update: storage counts while its item list has focus, not its popups.
void option_menu_list_update(void* list, bool focus) {
    if (!focus || !g_installed) {
        return;
    }
    const auto l = reinterpret_cast<std::uint64_t>(list);
    const std::uint64_t listener = read_u64(l + 0x30);
    if (!listener || read_u64(listener) != guest_at(kItemListListener)) {
        return;
    }
    const std::uint64_t name = open_menu_step_name();
    if (name == guest_at(kDepositoryName)) {
        g_storage_ms.store(now_ms(), std::memory_order_relaxed);
    }
}

// ---- bbhost's host interface (bbhost/compat.h) ----

// Through the kernel: an unmapped page fails instead of faulting.
bool host_read_safe(const void* addr, void* out, std::size_t len) {
#if defined(_WIN32)
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, len, &done) && done == len;
#else
    iovec local{out, len};
    iovec remote{const_cast<void*>(addr), len};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(len);
#endif
}

HostSettings host_settings() {
    return {BbSettings::Get().mouse_menu.load(std::memory_order_relaxed)};
}

PadState host_pad_state() {
    PadState p;
    p.buttons = g_pad_buttons.load(std::memory_order_relaxed);
    p.lx = g_pad_lx.load(std::memory_order_relaxed);
    p.ly = g_pad_ly.load(std::memory_order_relaxed);
    return p;
}

InputDevice host_input_device() {
    return g_pad_device.load() ? InputDevice::Pad : InputDevice::KeyboardMouse;
}

// The presenter letterboxes the 16:9 frame into the window; the game's UI stage is 1920x1080.
bool host_mouse_stage_position(float& x, float& y) {
    const int w = g_window_w.load(std::memory_order_relaxed);
    const int h = g_window_h.load(std::memory_order_relaxed);
    if (!g_inside.load() || !g_focused.load() || g_relative.load() || w <= 0 || h <= 0) {
        return false;
    }
    const float scale = std::min(w / 1920.0f, h / 1080.0f);
    x = (g_mouse_x.load(std::memory_order_relaxed) - (w - 1920.0f * scale) * 0.5f) / scale;
    y = (g_mouse_y.load(std::memory_order_relaxed) - (h - 1080.0f * scale) * 0.5f) / scale;
    return true;
}

bool host_mouse_relative() {
    return g_relative.load();
}

void host_mouse_describe(char* out, std::size_t n) {
    std::snprintf(out, n, "pointer %.0f,%.0f window %dx%d", static_cast<double>(g_mouse_x.load()),
                  static_cast<double>(g_mouse_y.load()), g_window_w.load(), g_window_h.load());
}

bool host_options_open() {
    return bbgpu_overlay_captures_input() != 0;
}

bool host_text_entry_open() {
    return bbgpu_text_input_is_active() != 0;
}

// ---- The window's side ----

namespace BbMenuPointer {

bool Active() {
    return g_installed && BbSettings::Get().mouse_menu.load(std::memory_order_relaxed) &&
           menu_pointer_in_menu();
}

void NoteMouse(float x, float y) {
    g_mouse_x.store(x, std::memory_order_relaxed);
    g_mouse_y.store(y, std::memory_order_relaxed);
}

void NoteWindow(int width, int height, bool focused, bool inside, bool relative) {
    g_window_w.store(width, std::memory_order_relaxed);
    g_window_h.store(height, std::memory_order_relaxed);
    g_focused.store(focused);
    g_inside.store(inside);
    g_relative.store(relative);
}

void NoteDevice(bool pad) {
    g_pad_device.store(pad);
}

void Click() {
    menu_pointer_click();
}

void Back() {
    push_pulse(menu_back_button());
}

void Wheel(float notches) {
    if (notches != 0.0f) {
        push_pulse(notches > 0.0f ? kPadUp : kPadDown);
    }
}

bool RightClickSwitchesPages() {
    const auto& s = BbSettings::Get();
    return now_ms() - g_storage_ms.load(std::memory_order_relaxed) < 250 && s.mk_enabled.load() &&
           s.keybinds[BB_ACTION_FIREARM].load() == BB_KEY_MOUSE_RIGHT;
}

} // namespace BbMenuPointer

// ---- The loader's and the pad's side ----

extern "C" int bbgpu_menu_pointer_install(unsigned char* image, uint64_t size) {
    if (const char* env = std::getenv("BB_MENU_POINTER"); env && env[0] == '0') {
        host_log("pointer: off (BB_MENU_POINTER=0)");
        return 0;
    }
    const std::uint64_t slot_off = kListVTable - kPreferredGuestSlide + kListUpdateSlot * 8;
    if (size < slot_off + 8) {
        host_log("pointer: refused - the image is smaller than expected");
        return 0;
    }
    const auto base = reinterpret_cast<std::uint64_t>(image);
    g_image_base = base;
    ElfImage elf{{image, static_cast<std::size_t>(size), base}};
    menu_pointer_install(&elf);
    std::uint64_t slot = 0;
    std::memcpy(&slot, image + slot_off, sizeof(slot));
    g_installed = slot != base + (kListUpdate - kPreferredGuestSlide);
    return g_installed ? 1 : 0;
}

extern "C" int bbgpu_menu_pointer_mouse_buttons(void) {
    if (!BbMenuPointer::Active() || g_relative.load()) {
        return 0;
    }
    return BbMenuPointer::RightClickSwitchesPages() ? 1 : 3;
}

extern "C" uint32_t bbgpu_menu_pointer_buttons(uint32_t buttons, uint8_t lx, uint8_t ly) {
    g_pad_buttons.store(buttons, std::memory_order_relaxed);
    g_pad_lx.store(lx, std::memory_order_relaxed);
    g_pad_ly.store(ly, std::memory_order_relaxed);
    static bool storage_was_open = false;
    if (const bool open = BbMenuPointer::RightClickSwitchesPages(); open != storage_was_open) {
        storage_was_open = open;
        host_log(open ? "pointer: Item Storage's item list - right click switches pages (L2)"
                      : "pointer: right click goes back again");
    }
    const std::uint32_t press = menu_pointer_take_press();
    if (!BbMenuPointer::Active()) {
        clear_pulses();
        return 0;
    }
    if (press) {
        push_pulse(press);
    }
    return take_pulse();
}
