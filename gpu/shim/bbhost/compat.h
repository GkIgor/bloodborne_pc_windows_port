// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the parts of bbhost's host interface that menu_pointer.cpp uses, mapped onto this
// port. Declarations follow bbhost's (core/memory.h, host/window.h, ...); the definitions not
// given here are in ../bbport_menu_pointer.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>

#define GUEST_ABI __attribute__((sysv_abi))

// The eboot's own base; this port's image holds it at offset 0, so the slide is the image base.
constexpr std::uint64_t kPreferredGuestSlide = 0x400000ull;

struct GuestMemory {
    void* base = nullptr;
    std::size_t size = 0;
    std::uint64_t slide = 0;
};
struct ElfImage {
    GuestMemory mem;
};

inline void* guest_ptr(const GuestMemory& mem, std::uint64_t va) {
    return static_cast<std::uint8_t*>(mem.base) + (va - mem.slide);
}
// Hooks go in while the loader still has the image writable; it protects the segments afterwards.
inline bool guest_protect_rx(GuestMemory*, std::uint64_t, std::size_t) { return true; }
inline bool guest_protect_rw(GuestMemory*, std::uint64_t, std::size_t) { return true; }
inline bool guest_protect_rwx(GuestMemory*, std::uint64_t, std::size_t) { return true; }

// Guest code calls GUEST_ABI host functions directly; no wrapper is needed.
inline void* hle_wrap_fn(void* fn) { return fn; }

template <typename T>
std::int64_t hle_guest_arg(T value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::int64_t>(value);
    } else {
        return static_cast<std::int64_t>(value);
    }
}
// Calls a guest function with up to six integer or pointer arguments.
template <typename T = std::int64_t, typename... A>
T hle_call_guest(void* fn, A... args) {
    static_assert(sizeof...(A) <= 6);
    using Fn = std::int64_t(GUEST_ABI*)(std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                        std::int64_t, std::int64_t);
    const std::int64_t v[7] = {0, hle_guest_arg(args)...};
    return static_cast<T>(reinterpret_cast<Fn>(fn)(v[1], v[2], v[3], v[4], v[5], v[6]));
}

bool host_read_safe(const void* addr, void* out, std::size_t len);

template <typename... Args>
void host_log(const char* fmt, Args&&... args) {
    std::fputs("Menu ", stdout);
    if constexpr (sizeof...(Args) == 0) {
        std::fputs(fmt, stdout);
    } else {
        std::printf(fmt, std::forward<Args>(args)...);
    }
    std::putchar('\n');
    std::fflush(stdout);
}

struct PadState {
    std::uint32_t buttons = 0;
    std::uint8_t lx = 128, ly = 128;
};
enum class InputDevice : int { KeyboardMouse = 0, Pad = 1 };
struct HostSettings {
    bool mouse_menu;
};

HostSettings host_settings();
PadState host_pad_state();
InputDevice host_input_device();
bool host_mouse_stage_position(float& x, float& y);
bool host_mouse_relative();
void host_mouse_describe(char* out, std::size_t n);
bool host_options_open();
bool host_text_entry_open();

// bbhost features this port does not have: its own host menu, the debug menu's controller and
// the native PC option rows.
inline bool ingame_menu_open() { return false; }
inline bool debug_menu_open() { return false; }
// bbport: spots Item Storage's item list (../bbport_menu_pointer.cpp).
void option_menu_list_update(void* list, bool focus);
inline bool option_menu_click_decides(void*, int) { return false; }
