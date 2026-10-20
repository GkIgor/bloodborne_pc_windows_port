# bbhost ports

Source: https://github.com/droogie/bbhost/tree/db5457cbd447f4ef129b58df0a78e0a8819c7033
(unchanged at 566184a), following DarkIzuku's bloodborne_pc port
(https://github.com/DarkIzuku/bloodborne_pc/tree/v0.4.1-stability.5).

The mouse as a pointer in the game's menus:

- `gpu/shim/bbhost/menu_pointer.cpp` and `menu_pointer.h` are bbhost's `src/engine/menu_pointer.cpp`
  and `.h`. Only their first lines differ: an SPDX/attribution header, and in the `.cpp` the include
  block, replaced by `compat.h`. They keep bbhost's list-update hook, Scaleform geometry hit tests,
  hover on movement, click handling (decide, slider arrows, combo box sides, spinners),
  region-aware confirmation and the front-end/open-menu checks.
- `gpu/shim/bbhost/compat.h` declares the parts of bbhost's host interface that file uses
  (guest memory and calls, logging, pad, mouse, settings) with bbhost's signatures; features this
  port lacks (bbhost's host menu, debug-menu controller, native PC option rows) are stubs.
- `gpu/shim/bbport_menu_pointer.cpp` and `.h` implement that interface for this port: the
  window's mouse and the pad, the loader hook-up, and the presses a click or the wheel makes.
  They are used from `gpu/shim/window.cpp`, `src/runtime_pad.c`, `src/probe.c` and `bbport.ini`
  `mouse_menu`.

These files are GPL-3.0-or-later; copyright in bbhost's code remains with the bbhost
contributors. The rest of this port remains GPL-2.0-or-later under its existing notices.
Builds containing these files are distributed under GPL-3.0-or-later; see `GPL-3.0.txt`.
Third-party notices remain unchanged.
