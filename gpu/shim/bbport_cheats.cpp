// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_cheats.h"

#include "bbport_settings.h"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// probe.c: the real mapped image (all linked modules), not the unused MemoryPatcher base.
extern "C" void runtime_guest_image_range(uint64_t* base, uint64_t* size);
extern "C" int runtime_guest_image_ready(void);

namespace BbCheats {
namespace {

constexpr uint64_t kPs4Base = 0x400000;

// The JSON master's first instruction stores the player object pointer at the PS4 scratch
// address 0x4000.  The 1 Hit Kill code cave reads that value to avoid one-shotting the
// player.  In the 1 Hit Kill code cave the PS4-relative displacement resolves to
// `image_base - 0x3FC000` in this port.  Derive the scratch address from the live image
// base instead of hardcoding it, so the master's absolute store and the code cave's
// RIP-relative read always agree.  Do not NOP the store: 1 Hit Kill depends on it.
constexpr uint64_t kJsonScratchDelta = 0x3FC000;
constexpr size_t kJsonScratchRegionSize = 0x10000;
uint64_t g_json_scratch = 0; ///< image_base - kJsonScratchDelta, set when the image is ready

struct Write {
    uint64_t offset = 0;
    std::vector<uint8_t> on;
    std::vector<uint8_t> off; ///< explicit JSON off, or the known original for XML
    bool hook = false;        ///< write after the code cave when enabling; restore first
    std::vector<uint8_t> saved;
    bool saved_valid = false;
};

struct Cheat {
    int id = -1;
    std::string name;
    std::string description;
    int category = CategoryPlayer;
    bool json = false;
    int effect_index = -1; ///< BbSettings::Effects entry, -1 when not a visual effect
    /// True for the "Disable X" visual cheats (cheat on -> effect off).  False for cheats
    /// whose setting has the same polarity as the cheat (Skip Intro).
    bool effect_inverted = true;
    std::vector<Write> writes;
    bool active = false;
    bool available = false;
    bool startup_applied = false;
    std::string error;
};

std::recursive_mutex g_mutex;
uint64_t g_image_base = 0;
uint64_t g_image_size = 0;
bool g_ready = false;
bool g_table_built = false;
bool g_master_applied = false;
bool g_scratch_ready = false;
std::vector<Cheat> g_cheats;
std::string g_status = "Live patcher waiting for the game image.";

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> Hex(const char* text) {
    std::vector<uint8_t> out;
    if (!text) return out;
    int high = -1;
    for (const char* p = text; *p; ++p) {
        const int nibble = HexNibble(*p);
        if (nibble < 0) continue;
        if (high < 0) {
            high = nibble;
        } else {
            out.push_back(uint8_t((high << 4) | nibble));
            high = -1;
        }
    }
    return out;
}

Write JsonWrite(uint64_t offset, const char* on, const char* off, bool hook = false) {
    Write w;
    w.offset = offset;
    w.on = Hex(on);
    w.off = Hex(off);
    w.hook = hook;
    return w;
}

Write JsonZero(uint64_t offset, const char* on, bool hook = false) {
    Write w;
    w.offset = offset;
    w.on = Hex(on);
    w.off.assign(w.on.size(), 0);
    w.hook = hook;
    return w;
}

Write XmlWrite(uint64_t address, const char* on, const char* off) {
    Write w;
    w.offset = address >= kPs4Base ? address - kPs4Base : address;
    w.on = Hex(on);
    w.off = Hex(off);
    return w;
}

int EffectIndex(const char* key) {
    if (!key) return -1;
    for (int i = 0; i < BbSettings::EffectCount; ++i) {
        if (std::strcmp(BbSettings::Effects[i].key, key) == 0) return i;
    }
    return -1;
}

size_t PageSize() {
    static const size_t page = [] {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return size_t(info.dwPageSize);
#else
        return size_t(sysconf(_SC_PAGESIZE));
#endif
    }();
    return page;
}

// Reserve the scratch page that the JSON master and the 1 Hit Kill code cave share.  This
// runs before the cheat table is validated, so the validator accepts the two PS4-derived
// references instead of disabling the patch.
bool EnsureJsonScratchLocked() {
    if (g_scratch_ready) return true;
    if (!g_json_scratch) {
        g_status = "The JSON scratch address is not available.";
        return false;
    }
#ifdef _WIN32
    auto* const target = reinterpret_cast<void*>(static_cast<uintptr_t>(g_json_scratch));
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(target, &info, sizeof(info))) {
        g_status = "Could not query the JSON scratch page.";
        return false;
    }
    // Do not take over a page that is already committed by the host or a module.  The
    // address is below the guest low-map reservation (0x0800000000), so it is free in a
    // normal session; if it is not, fail closed instead of corrupting unrelated memory.
    if (info.State == MEM_COMMIT) {
        g_status = "The JSON scratch address is already in use.";
        return false;
    }
    if (info.Type == MEM_IMAGE || info.Type == MEM_MAPPED) {
        g_status = "The JSON scratch address belongs to a loaded image.";
        return false;
    }
    if (info.State == MEM_RESERVE) {
        if (!VirtualAlloc(target, PageSize(), MEM_COMMIT, PAGE_READWRITE)) {
            char text[192];
            std::snprintf(text, sizeof(text),
                          "Could not commit the JSON scratch page (Win32 error %lu).",
                          static_cast<unsigned long>(GetLastError()));
            g_status = text;
            return false;
        }
    } else {
        // The target is page-aligned but not allocation-granularity aligned.  Reserve the
        // containing 64 KiB granule, then commit it read/write.
        const uintptr_t region_base =
            static_cast<uintptr_t>(g_json_scratch) & ~uintptr_t(0xFFFF);
        void* const region = VirtualAlloc(reinterpret_cast<void*>(region_base),
                                          kJsonScratchRegionSize,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!region) {
            char text[192];
            std::snprintf(text, sizeof(text),
                          "Could not reserve the JSON scratch page at 0x%llX "
                          "(Win32 error %lu).",
                          static_cast<unsigned long long>(g_json_scratch),
                          static_cast<unsigned long>(GetLastError()));
            g_status = text;
            return false;
        }
        std::memset(region, 0, kJsonScratchRegionSize);
    }

    MEMORY_BASIC_INFORMATION verify{};
    if (!VirtualQuery(target, &verify, sizeof(verify)) || verify.State != MEM_COMMIT ||
        (verify.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(verify.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY))) {
        g_status = "The JSON scratch page is not writable.";
        return false;
    }
    g_scratch_ready = true;
    return true;
#else
    g_status = "The JSON scratch page is not implemented on this platform.";
    return false;
#endif
}

// Reject code that dereferences an address that is not mapped/readable/writable at the
// moment the patch is applied.  The 1 Hit Kill code cave originally crashed on a PS4-only
// absolute address; the scratch page above makes its reference valid, and this check keeps
// any future table entry from repeating the same failure mode.
bool ValidateCodeReferences(const Write& write, std::string* error) {
    if (write.on.empty()) return true;
#ifdef _WIN32
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                     ZYDIS_STACK_WIDTH_64))) {
        if (error) *error = "Could not initialize the instruction decoder.";
        return false;
    }
    constexpr DWORD readable =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    size_t pos = 0;
    while (pos < write.on.size()) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, write.on.data() + pos,
                                               write.on.size() - pos, &instruction,
                                               operands))) {
            // The table also contains raw XML values and strings.  Data has no instruction
            // stream to validate, so stop at the first undecodable byte.
            break;
        }
        const uint64_t runtime = g_image_base + write.offset + pos;
        const bool is_lea = instruction.mnemonic == ZYDIS_MNEMONIC_LEA;
        for (ZyanU8 i = 0; i < instruction.operand_count; ++i) {
            const ZydisDecodedOperand& operand = operands[i];
            if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || is_lea) continue;
            ZyanU64 target = 0;
            if (ZYAN_FAILED(
                    ZydisCalcAbsoluteAddress(&instruction, &operand, runtime, &target))) {
                // Register-relative access: the base is a live guest pointer, so its
                // target cannot be checked statically.
                continue;
            }
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(target)), &info,
                              sizeof(info)) ||
                info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                char text[256];
                std::snprintf(text, sizeof(text),
                              "Code at image offset 0x%llX references unmapped address "
                              "0x%llX; patch disabled for safety.",
                              static_cast<unsigned long long>(write.offset + pos),
                              static_cast<unsigned long long>(target));
                if (error) *error = text;
                return false;
            }
            const ZyanU16 actions = static_cast<ZyanU16>(operand.actions);
            if ((actions & ZYDIS_OPERAND_ACTION_MASK_READ) &&
                !(info.Protect & readable)) {
                char text[256];
                std::snprintf(text, sizeof(text),
                              "Code at image offset 0x%llX reads non-readable address "
                              "0x%llX; patch disabled for safety.",
                              static_cast<unsigned long long>(write.offset + pos),
                              static_cast<unsigned long long>(target));
                if (error) *error = text;
                return false;
            }
            if ((actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
                !(info.Protect & writable)) {
                char text[256];
                std::snprintf(text, sizeof(text),
                              "Code at image offset 0x%llX writes non-writable address "
                              "0x%llX; patch disabled for safety.",
                              static_cast<unsigned long long>(write.offset + pos),
                              static_cast<unsigned long long>(target));
                if (error) *error = text;
                return false;
            }
        }
        pos += instruction.length;
        if (instruction.length == 0) break;
    }
    return true;
#else
    (void)write;
    (void)error;
    return true;
#endif
}

bool RangeValid(uint64_t offset, size_t length) {
    if (!g_image_base || !g_image_size) return false;
    if (length == 0) return true;
    return offset <= g_image_size && uint64_t(length) <= g_image_size - offset;
}

bool ReadBytes(uint64_t offset, uint8_t* out, size_t length) {
    if (!RangeValid(offset, length)) return false;
    auto* src = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(g_image_base + offset));
#ifdef _WIN32
    // probe.c maps the whole image PAGE_NOACCESS, then makes only the six ELF segments
    // readable.  The JSON code caves live in the gaps between those segments and therefore
    // stay PAGE_NOACCESS.  DetectStates() must read them to learn whether a patch is live,
    // so temporarily make no-access pages readable instead of faulting.
    const size_t page = PageSize();
    size_t done = 0;
    while (done < length) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(src) + done;
        const size_t chunk = std::min<size_t>(length - done, page - address % page);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT) return false;
        if ((info.Protect & PAGE_NOACCESS) || (info.Protect & PAGE_GUARD)) {
            DWORD old_protect = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(address), chunk, PAGE_READONLY,
                                &old_protect)) {
                return false;
            }
            std::memcpy(out + done, reinterpret_cast<void*>(address), chunk);
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(address), chunk, old_protect, &ignored);
        } else {
            std::memcpy(out + done, reinterpret_cast<void*>(address), chunk);
        }
        done += chunk;
    }
    return true;
#else
    std::memcpy(out, src, length);
    return true;
#endif
}

std::vector<uint8_t> ReadVector(uint64_t offset, size_t length) {
    std::vector<uint8_t> data(length);
    if (!ReadBytes(offset, data.data(), length)) data.clear();
    return data;
}

bool WriteBytes(uint64_t offset, const std::vector<uint8_t>& data) {
    if (data.empty()) return true;
    if (!RangeValid(offset, data.size())) return false;
    auto* dst = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(g_image_base + offset));
#ifdef _WIN32
    const size_t page = PageSize();
    size_t done = 0;
    while (done < data.size()) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(dst) + done;
        const size_t chunk = std::min<size_t>(data.size() - done, page - address % page);
        DWORD old_protect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), chunk, PAGE_EXECUTE_READWRITE,
                            &old_protect)) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(address), data.data() + done, chunk);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), chunk);
        // Code caves live in the image's PAGE_NOACCESS gaps.  Restoring PAGE_NOACCESS after
        // writing would make the guest fault as soon as the hook jumps there, so leave
        // formerly inaccessible pages executable instead.
        DWORD restore = old_protect;
        if (restore == PAGE_NOACCESS || restore == 0) restore = PAGE_EXECUTE_READ;
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), chunk, restore, &ignored);
        done += chunk;
    }
#else
    const size_t page = PageSize();
    const uintptr_t begin = reinterpret_cast<uintptr_t>(dst) & ~uintptr_t(page - 1);
    const uintptr_t end =
        (reinterpret_cast<uintptr_t>(dst) + data.size() + page - 1) & ~uintptr_t(page - 1);
    if (mprotect(reinterpret_cast<void*>(begin), end - begin,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(dst, data.data(), data.size());
    __builtin___clear_cache(reinterpret_cast<char*>(dst),
                            reinterpret_cast<char*>(dst) + data.size());
#endif
    return true;
}

std::string OffsetError(const char* what, uint64_t offset) {
    char text[192];
    std::snprintf(text, sizeof(text), "%s at image offset 0x%llX is outside the game image.",
                  what, static_cast<unsigned long long>(offset));
    return text;
}

std::string WriteError(uint64_t offset) {
    char text[192];
    std::snprintf(text, sizeof(text),
                  "Memory write failed at image offset 0x%llX (not writable).",
                  static_cast<unsigned long long>(offset));
    return text;
}

void RefreshInfoLocked();

void AddJson(int id, const char* name, const char* description, int category,
             std::vector<Write> writes) {
    Cheat cheat;
    cheat.id = id;
    cheat.name = name;
    cheat.description = description;
    cheat.category = category;
    cheat.json = true;
    cheat.writes = std::move(writes);
    g_cheats.push_back(std::move(cheat));
}

void AddXml(int id, const char* name, const char* description, int category,
            const char* effect_key, std::vector<Write> writes, bool effect_inverted = true) {
    Cheat cheat;
    cheat.id = id;
    cheat.name = name;
    cheat.description = description;
    cheat.category = category;
    cheat.json = false;
    cheat.effect_index = EffectIndex(effect_key);
    cheat.effect_inverted = effect_inverted;
    cheat.writes = std::move(writes);
    g_cheats.push_back(std::move(cheat));
}

const std::vector<Write>& MasterWrites() {
    static const std::vector<Write> writes = [] {
        std::vector<Write> list;
        // JSON master trampoline: code cave first, hook last.  It is never reverted.
        //
        // The database's first instruction is `mov [0x4000], rax`: it stores the player
        // object pointer at the PS4 scratch address that the 1 Hit Kill code cave reads.
        // Relocate the absolute store to the scratch address derived from this port's image
        // base (reserved by EnsureJsonScratchLocked) instead of NOPing it; the two patches
        // must agree.
        auto code = Hex(
            "48A300400000000000008B88F80000008988F80000008B88F800000041894C2418E981C081FC");
        if (g_json_scratch == 0 || code.size() < 10) return list;
        code[0] = 0x48;
        code[1] = 0xA3;
        for (int byte = 0; byte < 8; ++byte) {
            code[size_t(2 + byte)] = uint8_t((g_json_scratch >> (8 * byte)) & 0xFF);
        }
        Write trampoline;
        trampoline.offset = 0x50DB810;
        trampoline.on = std::move(code);
        list.push_back(std::move(trampoline));
        list.push_back(JsonWrite(0x18F78B1, "E95A3F7E0390", nullptr, true));
        return list;
    }();
    return writes;
}

void BuildTable() {
    g_cheats.clear();
    g_cheats.reserve(17);

    // CUSA03173_01.09.json.  These offsets are image offsets (verified against boot.bin).
    AddJson(0, "Infinite Health",
            "Locks your current HP at maximum. The JSON master hook is applied automatically.",
            CategoryPlayer,
            {
                JsonWrite(0x1BFC5F7, "C787F8000000FFFFFF7F90",
                          "8B97F80000004189542404", true),
            });
    AddJson(1, "Infinite Stamina", "Stamina does not drain while sprinting or attacking.",
            CategoryPlayer,
            {
                JsonZero(0x50DB836,
                         "8B88380100008988340100008B80340100004189442434E98FC081FC"),
                JsonWrite(0x18F78DB, "E9563F7E0390", "8B8034010000", true),
            });
    AddJson(2, "Infinite Blood Echoes", "Blood Echoes do not decrease when spent.",
            CategoryItems,
            {
                JsonZero(0x50DB800, "B9FFE0F505898A94000000E9914A82FC"),
                JsonWrite(0x190029B, "E960B57D0390", "898A94000000", true),
            });
    AddJson(3, "Infinite Insight (Lucidity)", "Insight does not decrease when spent.",
            CategoryItems,
            {
                JsonZero(0x50DB889,
                         "BEFFE0F50589B3840000008BB384000000E91B6782FC"),
                JsonWrite(0x1901FB4, "E9D0987D0390", "8BB384000000", true),
            });
    AddJson(4, "1 Hit Kill", "Enemies die from a single hit.", CategoryCombat,
            {
                JsonZero(0x50DB852,
                         "39F00F4EF0534889FB4881EBF0000000483B1D9787B2FA74065BE9111C93FC5BE90D1C93FC"),
                JsonWrite(0x1A0D47B, "E9D2E36C03", "39F00F4EF0", true),
            });
    AddJson(5, "Infinite Items", "Consumables and bullets are not consumed.",
            CategoryItems,
            {
                JsonZero(0x50DB877, "4189C441BC1400000044896308E9D4DC3FFC"),
                JsonWrite(0x14D9556, "E91C23C0039090", "4189C444896308", true),
            });

    // patches/Bloodborne.xml.  XML addresses are absolute PS4 addresses.
    AddXml(6, "Player No Dead",
           "You can take damage but never drop below 1 HP.", CategoryPlayer, nullptr,
           {XmlWrite(0x0593E888, "01000000", "00000000")});
    AddXml(7, "Player Stealth",
           "Enemies do not see or react to you unless attacked.", CategoryPlayer, nullptr,
           {XmlWrite(0x0593E88E, "01", "00")});
    AddXml(8, "Player Silent",
           "Enemies do not hear you, but can still see you.", CategoryPlayer, nullptr,
           {XmlWrite(0x0593E88F, "01", "00")});
    AddXml(9, "Enemy Control",
           "Take control of enemies (community patch; experimental).", CategoryCombat, nullptr,
           {
               XmlWrite(0x1C90E60,
                        "0f85ac0000004c8d2d0bdaca03498b4d004885c975318d3deda40a038d153ba50a038d"
                        "0de3a30a03beb100000031c04d8be64d8bf0e8164782004d8bc64d8bf4498b4d008b4364"
                        "83f8ff7466ba0e060000c4e268f7d0488b71088b761883c60239f27d4e8bd2488b8cd150"
                        "0800004885c9743f25ff3f00003b017d36486bc03848034108742c488b004885c0742441"
                        "5056575031f6488bf8c680880000001de80ccf0300585f5e4158837878070f94c0",
                        "0f85940000004c8d2d0bdaca03498b4d004885c97534488d3deca40a03488d1539a50a03"
                        "488d0de0a30a03beb100000031c04d89f44d89c6e8134782004d89f04d89e6498b4d008b"
                        "436483f8ff744fba0e060000c4e268f7d0488b71088b761883c60239f27d3b89d2488b8c"
                        "d1500800004885c9743025ff3f00003b017d2b486bc038480341087425488b004885c074"
                        "21837878070f94c0eb1a31c0eb1631c0eb1231c0eb0e31c0eb0a31c0eb0631c0"),
               XmlWrite(0x197296A,
                        "064889f34889f2488b33807e1900750c488d5e1048394e207cedebe54839c2741b4839"
                        "4a20488bc87f03488bca4839c1740ac6413001488b4128eb084831f6e8023bd9ff48c7"
                        "c617000000488bf8e8a3f2a1ff84c0741741505657504831ffbe28000000e83db4350058"
                        "5f5e41584183bc24c4000000010f85be0000004183bc24c8000000000f85af000000498b"
                        "3f4885ff7524488d3d64893c03488d15b1893c03488d0d27893c03beb100000031c0e891"
                        "2bb400498b3f488b4738488b4008488d5808488d0d8f3cfe03488bd0eb06488bde488bd6"
                        "488b33807e1900750c488d5e1048394e207cedebe54839c2741b48394a20488bc87f0348"
                        "8bca4839c1740ac6413001488b4128eb05e856020000488945c0488d05abd2d7034883c0"
                        "10488945c8c745d08988083d498bbc24d0000000488d75c0e80b1b7a00498b06483b45d8"
                        "750d4883c4205b415c415e415f5dc3e838ba6401",
                        "154889f34889f26666666666662e0f1f840000000000488b33807e1900750c488d5e1048"
                        "394e207cedebd64839c2741748394a204889c17f034889d14839c17406c6413001eb05e8"
                        "fa3ad9ff4183bc24c4000000010f85d00000004983bc24d0000000000f84c10000004183"
                        "bc24c8000000000f85b2000000498b3f4885ff7524488d3d77893c03488d15c4893c0348"
                        "8d0d3a893c03beb100000031c0e8a42bb400498b3f488b4738488b4008488d5808488d0d"
                        "a23cfe034889c2eb094889f34889f20f1f00488b33807e1900750c488d5e1048394e207c"
                        "edebe24839c2741b48394a204889c17f034889d14839c1740ac6413001488b4128eb05e8"
                        "66020000488945c0488d05bbd2d7034883c010488945c8c745d08988083d498bbc24d000"
                        "0000488d75c0e81b1b7a00498b06483b45d8750d4883c4205b415c415e415f5dc3e848ba"
                        "6401906666666666662e0f1f840000000000"),
           });
    AddXml(10, "Disable Rally", "Disables HP regain from rally.", CategoryCombat, nullptr,
           {XmlWrite(0x01966AEC, "41B601", "4188C6")});
    AddXml(11, "Sensitive Analog Input",
           "Raises analog stick sensitivity for easier running.", CategoryCombat, nullptr,
           {XmlWrite(0x04D27714, "9A99993F", "0000803F")});
    AddXml(12, "Skip Intro",
           "Zeros the intro logo strings.  Takes effect on the next launch of the intro.",
           CategoryCombat, "skip_intro",
           {
               XmlWrite(0x04D99138, "00000000", "4C006F00"),
               XmlWrite(0x04D99154, "00000000", "4C006F00"),
               XmlWrite(0x04D9916E, "00000000", "4C006F00"),
           },
           false); // skip_intro has the same polarity as this cheat: on -> 1, off -> 0.

    AddXml(13, "Disable Motion Blur", "Disables motion blur.", CategoryVisual,
           "effect_motion_blur", {XmlWrite(0x026C2549, "00", "01")});
    AddXml(14, "Disable Depth of Field", "Disables depth of field (DoF).", CategoryVisual,
           "effect_dof", {XmlWrite(0x025D7BBC, "E9DF00000090", "0F84DE000000")});
    AddXml(15, "Disable SSAO", "Disables SSAO shading.", CategoryVisual, "effect_ssao",
           {XmlWrite(0x026C2548, "00", "01")});
    AddXml(16, "Disable Chromatic Aberration", "Disables chromatic aberration.",
           CategoryVisual, "effect_chromatic_aberration",
           {XmlWrite(0x0269FAA8, "C783AC000000000000009090",
                     "8B8590F5FFFF8983AC000000")});
}

void MarkUnavailable(const char* reason) {
    for (auto& cheat : g_cheats) {
        cheat.available = false;
        cheat.error = reason;
    }
}

void MarkJsonUnavailable(const char* reason) {
    for (auto& cheat : g_cheats) {
        if (!cheat.json) continue;
        cheat.available = false;
        cheat.error = reason;
    }
}

void ValidateTable() {
    for (auto& cheat : g_cheats) {
        cheat.available = true;
        cheat.error.clear();
        for (auto& write : cheat.writes) {
            if (write.on.empty()) {
                cheat.available = false;
                cheat.error = "Patch has no on-bytes.";
                break;
            }
            if (!RangeValid(write.offset, write.on.size())) {
                cheat.available = false;
                cheat.error = OffsetError("Patch", write.offset);
                break;
            }
            if (!write.off.empty() && !RangeValid(write.offset, write.off.size())) {
                cheat.available = false;
                cheat.error = OffsetError("Original bytes", write.offset);
                break;
            }
            std::string code_error;
            if (!ValidateCodeReferences(write, &code_error)) {
                cheat.available = false;
                cheat.error = code_error;
                break;
            }
        }
    }
}

void DetectStates() {
    for (auto& cheat : g_cheats) {
        bool all_on = true;
        for (auto& write : cheat.writes) {
            const auto current = ReadVector(write.offset, write.on.size());
            if (current.size() != write.on.size() || current != write.on) {
                all_on = false;
                break;
            }
        }
        cheat.active = all_on;
        cheat.startup_applied = all_on;
        if (all_on) {
            for (auto& write : cheat.writes) {
                if (!write.off.empty()) {
                    write.saved = write.off;
                    write.saved_valid = true;
                }
            }
        }
    }
}

void RefreshInfoLocked() {
    // List() builds a copy, so there is nothing cached here.  Kept as a single place for
    // future UI state if needed.
}

bool EnsureMasterLocked() {
    if (g_master_applied) return true;
    if (!g_scratch_ready) {
        g_status = "The JSON scratch page is unavailable; JSON patches are disabled.";
        return false;
    }
    for (const auto& write : MasterWrites()) {
        if (!RangeValid(write.offset, write.on.size())) {
            g_status = OffsetError("JSON master", write.offset);
            return false;
        }
        std::string code_error;
        if (!ValidateCodeReferences(write, &code_error)) {
            g_status = code_error;
            return false;
        }
    }
    for (const auto& write : MasterWrites()) {
        if (!WriteBytes(write.offset, write.on)) {
            g_status = WriteError(write.offset);
            return false;
        }
    }
    g_master_applied = true;
    return true;
}

bool ApplyWriteLocked(Cheat& cheat, Write& write, bool enable) {
    std::vector<uint8_t> data;
    if (enable) {
        if (!cheat.json && !write.saved_valid) {
            write.saved = ReadVector(write.offset, write.on.size());
            if (write.saved.size() != write.on.size()) {
                cheat.error = OffsetError("Original bytes", write.offset);
                return false;
            }
            write.saved_valid = true;
        }
        data = write.on;
    } else {
        if (!cheat.json && write.saved_valid) {
            data = write.saved;
        } else if (!write.off.empty()) {
            data = write.off;
        } else {
            cheat.error = "No original bytes are available to revert this patch.";
            return false;
        }
    }
    if (!WriteBytes(write.offset, data)) {
        cheat.error = WriteError(write.offset);
        return false;
    }
    if (!enable && !cheat.json) write.saved_valid = false;
    return true;
}

bool ApplyCheatLocked(Cheat& cheat, bool enable, bool save) {
    cheat.error.clear();
    if (enable && cheat.json && !EnsureMasterLocked()) {
        cheat.error = g_status;
        return false;
    }

    std::vector<Write*> applied;
    const auto apply = [&](Write& write) {
        if (!ApplyWriteLocked(cheat, write, enable)) return false;
        applied.push_back(&write);
        return true;
    };

    bool ok = true;
    if (enable) {
        for (auto& write : cheat.writes) {
            if (!write.hook && !apply(write)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (auto& write : cheat.writes) {
                if (write.hook && !apply(write)) {
                    ok = false;
                    break;
                }
            }
        }
    } else {
        for (auto& write : cheat.writes) {
            if (write.hook && !apply(write)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (auto& write : cheat.writes) {
                if (!write.hook && !apply(write)) {
                    ok = false;
                    break;
                }
            }
        }
    }

    if (!ok) {
        if (enable) {
            // Undo the writes that were applied, newest first.
            for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                Write& write = **it;
                const std::vector<uint8_t>& original =
                    (!cheat.json && write.saved_valid) ? write.saved : write.off;
                if (!original.empty()) WriteBytes(write.offset, original);
            }
            cheat.active = false;
            cheat.startup_applied = false;
        } else if (!applied.empty()) {
            // The hook (or the first write) was restored: the cheat is effectively off.
            cheat.active = false;
            cheat.startup_applied = false;
            BbSettings::Get().cheats[cheat.id] = false;
            if (cheat.effect_index >= 0) {
                // The cheat is off; restore the startup setting's off value.
                BbSettings::Get().effects[cheat.effect_index] = cheat.effect_inverted;
            }
            if (save) BbSettings::Save();
        } else {
            // Nothing was restored: keep the cheat marked active so the user can retry.
            cheat.active = true;
        }
        return false;
    }

    cheat.active = enable;
    cheat.startup_applied = false;
    if (cheat.effect_index >= 0) {
        // "Disable X" cheats invert the effect; Skip Intro keeps the same polarity.
        BbSettings::Get().effects[cheat.effect_index] =
            cheat.effect_inverted ? !enable : enable;
    }
    BbSettings::Get().cheats[cheat.id] = enable;
    if (save) BbSettings::Save();
    return true;
}

void EnsureReadyLocked() {
    if (g_ready) return;
    if (!g_table_built) {
        BuildTable();
        g_table_built = true;
    }

    // The image range is published before relocations and startup patches finish.  Do not
    // touch it until the guest entry point is ready, otherwise a persisted cheat could be
    // overwritten by startup patches or capture half-relocated bytes.
    if (!runtime_guest_image_ready()) {
        MarkUnavailable("Waiting for the game to start...");
        g_status = "Waiting for the game to start...";
        return;
    }

    uint64_t base = 0;
    uint64_t size = 0;
    runtime_guest_image_range(&base, &size);
    if (!base || !size) {
        MarkUnavailable("Waiting for the game image...");
        g_status = "Waiting for the game image...";
        return;
    }
    if (size < 0x1000) {
        MarkUnavailable("The game image range is invalid.");
        g_status = "The game image range is invalid.";
        return;
    }

    g_image_base = base;
    g_image_size = size;
    g_json_scratch = base > kJsonScratchDelta ? base - kJsonScratchDelta : 0;

    const bool scratch_ok = EnsureJsonScratchLocked();
    ValidateTable();

    bool json_ok = scratch_ok;
    if (!scratch_ok) {
        MarkJsonUnavailable(g_status.c_str());
    } else {
        std::string master_error;
        if (MasterWrites().empty()) {
            master_error = "The JSON master patch is empty.";
        } else {
            for (const auto& write : MasterWrites()) {
                if (!RangeValid(write.offset, write.on.size())) {
                    master_error = OffsetError("JSON master", write.offset);
                    break;
                }
                if (!ValidateCodeReferences(write, &master_error)) break;
            }
        }
        if (!master_error.empty()) {
            json_ok = false;
            g_status = master_error;
            MarkJsonUnavailable(g_status.c_str());
        }
    }

    DetectStates();
    g_ready = true;
    if (json_ok) {
        char text[192];
        std::snprintf(text, sizeof(text), "Live patcher ready: image 0x%llX, %.1f MB.",
                      static_cast<unsigned long long>(g_image_base),
                      double(g_image_size) / (1024.0 * 1024.0));
        g_status = text;
    }
    RefreshInfoLocked();
}

Cheat* FindLocked(int id) {
    for (auto& cheat : g_cheats) {
        if (cheat.id == id) return &cheat;
    }
    return nullptr;
}

} // namespace

void Update() {
    std::scoped_lock lock{g_mutex};
    if (g_ready) return;
    EnsureReadyLocked();
    if (!g_ready) return;

    // Apply persisted cheats once the image is known.  Visual effects that were already
    // applied by patches.py at startup are detected as active and left alone.
    for (auto& cheat : g_cheats) {
        if (!BbSettings::Get().cheats[cheat.id].load() || cheat.active) continue;
        if (!cheat.available) continue;
        ApplyCheatLocked(cheat, true, false);
    }
}

bool Ready() {
    std::scoped_lock lock{g_mutex};
    return g_ready;
}

const char* Status() {
    std::scoped_lock lock{g_mutex};
    return g_status.c_str();
}

std::vector<CheatInfo> List() {
    std::scoped_lock lock{g_mutex};
    if (!g_table_built) {
        BuildTable();
        g_table_built = true;
        MarkUnavailable("Waiting for the game image...");
    }
    std::vector<CheatInfo> list;
    list.reserve(g_cheats.size());
    for (const auto& cheat : g_cheats) {
        list.push_back({cheat.id, cheat.name, cheat.description, cheat.category, cheat.active,
                        cheat.available, cheat.startup_applied, cheat.error});
    }
    return list;
}

bool Set(int id, bool enabled) {
    std::scoped_lock lock{g_mutex};
    EnsureReadyLocked();
    Cheat* cheat = FindLocked(id);
    if (!cheat) return false;
    if (!g_ready) {
        cheat->error = g_status;
        return false;
    }
    if (!cheat->available) {
        if (cheat->error.empty()) cheat->error = "This patch is outside the game image.";
        return false;
    }
    if (cheat->active == enabled) return true;
    return ApplyCheatLocked(*cheat, enabled, true);
}

void EnableAll() {
    Update();
    const auto list = List();
    for (const auto& cheat : list) {
        if (cheat.available) Set(cheat.id, true);
    }
}

void DisableAll() {
    Update();
    const auto list = List();
    for (const auto& cheat : list) {
        if (cheat.active) Set(cheat.id, false);
    }
}

} // namespace BbCheats
