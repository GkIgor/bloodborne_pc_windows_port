// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the mouse as a pointer in the game's own menus (hover selects a row, click decides).
// The window's side of bbhost's menu pointer (bbhost/menu_pointer.cpp); see licenses/BBHOST-PORTS.md.

#pragma once

namespace BbMenuPointer {

/// The pointer drives the game's menus now: the window keeps the cursor instead of capturing it.
bool Active();

/// Window thread: the cursor in window coordinates and the window's state.
void NoteMouse(float x, float y);
void NoteWindow(int width, int height, bool focused, bool inside, bool relative);
/// The last input came from a gamepad (true) or the keyboard and mouse (false).
void NoteDevice(bool pad);

/// Window thread, while Active(): a left click, a right click and wheel notches.
void Click();
void Back();
void Wheel(float notches);

/// Item Storage's item list has focus and right click is bound to L2, which switches its pages.
bool RightClickSwitchesPages();

} // namespace BbMenuPointer
