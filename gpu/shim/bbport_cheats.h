// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: live cheat / debug patches for Bloodborne 01.09 (F4 menu).
//
// The JSON database (patches/CUSA03173_01.09.json) uses image offsets and explicit on/off
// bytes.  The XML database (patches/Bloodborne.xml) uses absolute PS4 addresses, so its
// offsets are converted with -0x400000.  XML patches that have no "off" string keep the
// bytes seen before the first write; for patches that may already be applied at startup
// (the effect_* settings in bbport.ini) the original bytes from the game image are used.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BbCheats {

enum Category : int {
    CategoryPlayer = 0, ///< Player / God Mode
    CategoryItems,      ///< Items, Echoes & Insight
    CategoryCombat,     ///< Combat & World
    CategoryVisual,     ///< Visual & Graphic Tweaks
    CategoryCount,
};

struct CheatInfo {
    int id;
    std::string name;
    std::string description;
    int category;
    bool active;
    bool available;
    bool startup_applied;
    std::string error;
};

/// Resolves the live guest image and applies persisted cheats once.  Cheap after the first
/// successful call; safe to call every frame.
void Update();

/// True once the guest image range is known and the patch table is validated.
bool Ready();

/// Human-readable state of the live patcher (for the menu header).
const char* Status();

/// A copy of the current table, in display order.
std::vector<CheatInfo> List();

/// Enables or disables one cheat.  Returns false and leaves `error` set when the write is
/// outside the image or the memory cannot be made writable.
bool Set(int id, bool enabled);

/// Enables every available cheat, then disables every active one.
void EnableAll();
void DisableAll();

} // namespace BbCheats
