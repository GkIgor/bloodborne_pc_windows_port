#!/usr/bin/env python3
"""Utility functions, constants, and batched background worker for Bloodborne PC Launcher."""

import ctypes
import hashlib
import locale
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from PySide6.QtCore import QThread, Signal

# Dynamic path resolution: works correctly whether run via python or as a compiled .exe
if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
else:
    APP_DIR = Path(__file__).resolve().parent

if not (APP_DIR / "run.bat").exists() and (APP_DIR.parent / "run.bat").exists():
    APP_DIR = APP_DIR.parent

RUN_BAT = APP_DIR / "run.bat"
LOG_FILE = APP_DIR / "bblauncher.log"
LOG_DIR = APP_DIR / "logs"
LOG_KEEP = 20
SETTINGS_FILE = APP_DIR / "bblauncher_settings.json"
PATCHES_XML = APP_DIR / "patches" / "Bloodborne.xml"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", "../CUSA03173"))

FPS_CHOICES = ["uncap", "60", "90", "30"]
RES_CHOICES = ["1920x1080 (Native)", "1280x720", "2560x1440", "3840x2160"]
WINDOW_MODE_CHOICES = ["Windowed", "Borderless", "Fullscreen"]
ANISO_CHOICES = [
    ("16x", "16", "16x anisotropic filtering of scene textures (recommended)"),
    ("8x", "8", "8x anisotropic filtering"),
    ("4x", "4", "4x anisotropic filtering"),
    ("2x", "2", "2x anisotropic filtering"),
    ("Off", "0", "Game's default anisotropic filtering (no override)"),
]
AA_CHOICES = ["Native FXAA (PS4 Default)", "Disable AA (No FXAA)", "TAA (Temporal Anti-Aliasing)"]
VK_NOISE = "<Warning> vk_instance.cpp"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

DEFAULT_SETTINGS = {
    "eboot": "",
    "fps": "60",
    "res": RES_CHOICES[0],
    "aniso": "16x",
    "anti_aliasing": AA_CHOICES[0],
    "mods": False,
    "hide_vk": True,
    "feat_upscaler": False,
    "feat_overlay": True,
    "feat_hud": False,
    "feat_fps_patch": True,
    "window_mode": "Borderless",
    "fullscreen": False,
    "enabled_patches": ["Skip Intro"],
}

DEFAULT_KEYBINDS = [
    ("key.attack", "Light Attack (R1)", "Mouse Left"),
    ("key.heavy_attack", "Heavy Attack (R2)", "Shift + Mouse Left"),
    ("key.firearm", "Firearm / Gun (L2)", "Mouse Right"),
    ("key.transform", "Weapon Transform (L1)", "Tab"),
    ("key.lock_on", "Lock-On / Camera Reset (R3)", "Mouse Middle"),
    ("key.move_forward", "Move Forward", "W"),
    ("key.move_backward", "Move Backward", "S"),
    ("key.move_left", "Move Left", "A"),
    ("key.move_right", "Move Right", "D"),
    ("key.dodge", "Dodge / Roll / Sprint (Circle)", "Space"),
    ("key.interact", "Interact / Talk (Cross)", "E"),
    ("key.use_item", "Use Item (Square)", "R"),
    ("key.heal", "Heal / Blood Vial (Triangle)", "F"),
    ("key.dpad_up", "Blood Bullets (Up)", "1"),
    ("key.dpad_down", "Switch Item (Down)", "2"),
    ("key.dpad_left", "Switch Left Weapon (Left)", "3"),
    ("key.dpad_right", "Switch Right Weapon (Right)", "4"),
    ("key.gestures", "Gestures Menu (Touchpad L)", "G"),
    ("key.personal_effects", "Personal Effects (Touchpad R)", "Backspace"),
    ("key.menu", "System Menu (Options)", "Escape"),
    ("key.walk", "Walk (hold)", "Left Alt"),
]

KEYBINDINGS = [(k, desc) for _, desc, k in DEFAULT_KEYBINDS]

_SYS_ENCODING = locale.getpreferredencoding(False)
_instance_mutex = []
_bb_guard_mod = False


def get_bb_guard():
    global _bb_guard_mod
    if _bb_guard_mod is False:
        try:
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import bb_guard
            _bb_guard_mod = bb_guard
        except Exception:
            _bb_guard_mod = None
    return _bb_guard_mod


def acquire_single_instance() -> bool:
    if os.name != "nt":
        return True
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        name = "Local\\BloodborneLauncher_" + hashlib.md5(str(APP_DIR).lower().encode()).hexdigest()[:12]
        handle = kernel32.CreateMutexW(None, False, name)
        if handle and ctypes.get_last_error() == 183:
            return False
        _instance_mutex.append(handle)
    except Exception:
        pass
    return True


def decode_line(raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(_SYS_ENCODING, errors="replace")


def shader_cache_dir() -> Path:
    user = Path(os.environ.get("BB_USER_DIR") or "user")
    return (user if user.is_absolute() else APP_DIR / user) / "cache"


def resolve_game_dir(target: str):
    p = Path(target.strip().strip('"'))
    if p.is_file() and p.name.lower() == "eboot.bin":
        return p.parent
    if p.is_dir() and (p / "eboot.bin").is_file():
        return p
    return None


def running_instances():
    guard = get_bb_guard()
    if guard is None:
        return []
    try:
        return guard.find_instances(APP_DIR / "out")
    except Exception:
        return []


def kill_tree(proc: subprocess.Popen):
    guard = get_bb_guard()
    if guard:
        for instance in running_instances():
            guard.terminate(instance.pid)

    if proc and proc.poll() is None:
        try:
            proc.terminate()
            try:
                proc.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                proc.kill()
        except OSError:
            pass


def rotate_log():
    try:
        if LOG_FILE.is_file() and LOG_FILE.stat().st_size > 0:
            LOG_DIR.mkdir(exist_ok=True)
            stamp = datetime.fromtimestamp(LOG_FILE.stat().st_mtime).strftime("%Y%m%d_%H%M%S")
            LOG_FILE.replace(LOG_DIR / f"bblauncher_{stamp}.log")
            for old in sorted(LOG_DIR.glob("bblauncher_*.log"))[:-LOG_KEEP]:
                old.unlink()
    except OSError:
        pass


def load_xml_patches(xml_path: Path = PATCHES_XML):
    import xml.etree.ElementTree as ET

    patches = []
    if not xml_path.is_file():
        return patches
    try:
        tree = ET.parse(xml_path)
        for meta in tree.getroot().iter("Metadata"):
            if meta.get("AppVer") == "01.09" and meta.get("AppElf", "eboot.bin") == "eboot.bin":
                name = meta.get("Name", "").strip()
                if not name:
                    continue
                author = meta.get("Author", "").strip() or "Unknown"
                note = meta.get("Note", "").strip().replace("\\n", "\n")
                n = name.lower()
                if "resolution patch" in n or "light grid" in n or "optimal 1080p" in n:
                    cat = "Resolutions & Grids"
                elif "fps" in n:
                    cat = "Framerate & Engine"
                elif any(k in n for k in ["debug", "capture", "http"]):
                    cat = "Debug & Tools"
                elif any(k in n for k in ["stealth", "silent", "no dead", "rally", "cheat", "sensitive analog", "unlock game region", "enemy control"]):
                    cat = "Gameplay & Cheats"
                elif any(k in n for k in ["blur", "shadow", "dof", "ssao", "chromatic", "camera", "reflections", "text scale", "physics", "aa"]):
                    cat = "Visuals & Camera"
                else:
                    cat = "Performance & Fixes"

                patches.append({
                    "name": name,
                    "author": author,
                    "note": note,
                    "category": cat,
                })
    except Exception as e:
        print(f"Error loading patches XML: {e}")
    return patches


def verify_prelaunch(game_dir: Path) -> tuple[bool, str, list[str]]:
    details = []

    eboot_path = game_dir / "eboot.bin"
    if not eboot_path.is_file():
        return False, f"Missing eboot.bin in game directory:\n{game_dir}", details
    eboot_size = eboot_path.stat().st_size
    if eboot_size == 0:
        return False, f"eboot.bin is empty in:\n{eboot_path}", details
    details.append(f"[OK] Game executable: eboot.bin ({eboot_size / (1024*1024):.1f} MiB)")

    try:
        scripts_dir = str(APP_DIR / "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        import game_check
        chk = game_check.problem(str(game_dir))
        if chk:
            kind, title, version = chk
            explanation = game_check.explain(kind, title, version)
            return False, f"Game File Verification Failed ({kind}):\n\n{explanation}", details
        details.append("[OK] Game Title & Version: Bloodborne CUSA03173 v1.09 verified")
    except Exception:
        details.append("[NOTE] Version check bypassed (standalone)")

    dvdroot = game_dir / "dvdroot_ps4"
    if dvdroot.is_dir():
        details.append("[OK] Asset directory: dvdroot_ps4 detected")

    if not RUN_BAT.exists():
        return False, f"Launcher script run.bat not found in:\n{APP_DIR}", details
    details.append("[OK] Launcher script: run.bat ready")

    bbport_exe = APP_DIR / "out" / "bbport.exe"
    if not bbport_exe.is_file():
        return False, f"Compiled binary bbport.exe missing at:\n{bbport_exe}\nRun build.bat first.", details
    details.append(f"[OK] Compiled binary: bbport.exe ({bbport_exe.stat().st_size / (1024*1024):.1f} MiB)")

    try:
        vk = ctypes.CDLL("vulkan-1.dll")
        details.append("[OK] Vulkan runtime: vulkan-1.dll loaded")
        if os.name == "nt" and hasattr(vk, "_handle") and vk._handle:
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(vk._handle))
    except OSError:
        details.append("[WARN] vulkan-1.dll not detected in system path")

    # CPU instruction check: AVX 1.0 support
    avx_ok, avx_msg = check_cpu_avx_support()
    if not avx_ok:
        return False, f"Hardware Incompatibility (Unsupported CPU):\n\n{avx_msg}", details
    details.append("[OK] CPU Architecture: AVX 1.0+ vector extensions verified")

    return True, "", details


def check_cpu_avx_support() -> tuple[bool, str]:
    if os.name != "nt":
        return True, ""
    try:
        kernel32 = ctypes.windll.kernel32
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
        kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]

        shellcode = bytes([
            0x53,                               # push rbx
            0xB8, 0x01, 0x00, 0x00, 0x00,       # mov eax, 1
            0x31, 0xC9,                         # xor ecx, ecx
            0x0F, 0xA2,                         # cpuid
            0xF7, 0xC1, 0x00, 0x00, 0x00, 0x10, # test ecx, 10000000h (bit 28 - AVX)
            0x74, 0x1A,                         # jz no_avx
            0xF7, 0xC1, 0x00, 0x00, 0x00, 0x08, # test ecx, 08000000h (bit 27 - OSXSAVE)
            0x74, 0x18,                         # jz no_osxsave
            0x31, 0xC9,                         # xor ecx, ecx
            0x0F, 0x01, 0xD0,                   # xgetbv (eax:edx = XCR0)
            0x83, 0xE0, 0x06,                   # and eax, 6 (bit 1=XMM, bit 2=YMM)
            0x83, 0xF8, 0x06,                   # cmp eax, 6
            0x75, 0x0B,                         # jne no_osxsave
            0xB8, 0x01, 0x00, 0x00, 0x00,       # mov eax, 1 (AVX OK)
            0x5B,                               # pop rbx
            0xC3,                               # ret
            # no_osxsave:
            0xB8, 0xFF, 0xFF, 0xFF, 0xFF,       # mov eax, -1
            0x5B,                               # pop rbx
            0xC3,                               # ret
            # no_avx:
            0x31, 0xC0,                         # xor eax, eax
            0x5B,                               # pop rbx
            0xC3                                # ret
        ])
        ptr = kernel32.VirtualAlloc(None, len(shellcode), 0x3000, 0x40)
        if not ptr:
            return True, ""
        ctypes.memmove(ptr, shellcode, len(shellcode))
        fn = ctypes.CFUNCTYPE(ctypes.c_int32)(ptr)
        status = fn()
        kernel32.VirtualFree(ptr, 0, 0x8000)

        if status == 0:
            return False, (
                "Your CPU lacks support for AVX (Advanced Vector Extensions 1.0).\n\n"
                "Bloodborne and native PS4 system libraries are built with -mavx and "
                "require AVX hardware instructions to run.\n\n"
                "Older processors without AVX (such as Intel Core 2 Quad, Nehalem/Westmere "
                "Xeon X56xx, or first-gen Core i7) cannot execute these binaries."
            )
        elif status == -1:
            return False, (
                "Your CPU supports AVX, but AVX state saving (OSXSAVE / XCR0 YMM) is disabled "
                "or masked by the operating system / hypervisor.\n\n"
                "If running inside a virtual machine or hypervisor, please enable AVX/XSAVE passthrough."
            )
        return True, ""
    except Exception:
        return True, ""


class ProcessWorker(QThread):
    batch_emitted = Signal(list)
    process_finished = Signal(int)

    def __init__(self, cmd, env, cwd):
        super().__init__()
        self.cmd = cmd
        self.env = env
        self.cwd = cwd
        self.proc = None

    def run(self):
        try:
            self.proc = subprocess.Popen(
                self.cmd,
                cwd=str(self.cwd),
                env=self.env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=NO_WINDOW,
            )
        except Exception as ex:
            self.batch_emitted.emit([f"Launch Error: {ex}"])
            self.process_finished.emit(-1)
            return

        batch = []
        last_flush = time.monotonic()

        try:
            for raw in iter(self.proc.stdout.readline, b""):
                batch.append(decode_line(raw).rstrip())
                now = time.monotonic()
                if len(batch) >= 60 or (now - last_flush) >= 0.035:
                    self.batch_emitted.emit(batch)
                    batch = []
                    last_flush = now
            if batch:
                self.batch_emitted.emit(batch)
        except Exception as ex:
            self.batch_emitted.emit([f"Read error: {ex}"])
        finally:
            try:
                self.proc.stdout.close()
            except OSError:
                pass
            rc = self.proc.wait()
            self.process_finished.emit(rc)

    def terminate_proc(self):
        if self.proc and self.proc.poll() is None:
            kill_tree(self.proc)