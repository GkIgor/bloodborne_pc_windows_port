# bbport DLSS bridge

`bbport_dlss.dll` — the only part of bbport that links NVIDIA's DLSS SDK. The port itself
(`bbport.exe`, `src/`, `gpu/`) never includes NGX headers or libraries: `vk_dlss.cpp` loads this
DLL at run time with `LoadLibraryA("bbport_dlss.dll")` and calls the plain C ABI in
`bbport_dlss_bridge.h`. Without the DLL, without an RTX GPU or with `BB_DLSS=0` the port keeps
FSR 3.1 and changes nothing else.

The code is adapted from `IFreemz/shadPS4-Bloodborne-DLSS-FSR` (MIT, `LICENSE.txt`).

## Why MSVC and not MinGW

NVIDIA's Windows SDK ships `nvsdk_ngx_s.lib` (and the debug variants) built with MSVC. It
references the MSVC C++ runtime and exception-handling metadata, so MinGW-w64's `ld` cannot link
it (undefined `__std_*`, `__security_cookie`, SEH records). Verified with gcc 15 / binutils 2.44:
the failure is intrinsic, not a missing flag. The bridge is therefore the one target built with
MSVC (`cl.exe`); everything else stays MinGW-w64.

`build.bat` performs the MSVC build automatically when Visual Studio (Build Tools or IDE) and the
vendored SDK are present, and warns + keeps FSR 3.1 otherwise.

## SDK files kept in the tree

Only the public, redistributable part of <https://github.com/NVIDIA/DLSS> is vendored, in
`third_party/nvngx-sdk/`:

```
include/nvsdk_ngx.h
include/nvsdk_ngx_defs.h
include/nvsdk_ngx_helpers.h
include/nvsdk_ngx_helpers_dlssd.h
include/nvsdk_ngx_helpers_vk.h
include/nvsdk_ngx_params.h
include/nvsdk_ngx_vk.h
lib/Windows_x86_64/x64/nvsdk_ngx_s.lib
```

To use a full SDK checkout instead, pass `-DDLSS_SDK_ROOT=<path>` to CMake
(the `.downloads/DLSS` sparse checkout used during development works).

**`nvngx_dlss.dll` is not part of this repository** (NVIDIA's runtime, ~59 MB). The NVIDIA RTX
SDKs license allows shipping it as part of a DLSS-enabled application together with the license
text, but `build.bat` never copies it into `out/`: place it there or in `<user>/dlss/` yourself
(or add it to your own release archive, keeping `licenses/NVIDIA-DLSS-LICENSE.txt`).

## Build

From a normal shell (the script finds VS with `vswhere` and calls `vcvars64.bat`):

```
build.bat
```

or manually, from an *x64 Native Tools Command Prompt* (or after `vcvars64.bat`):

```
cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CXX_COMPILER=cl ^
      -DDLSS_SDK_ROOT=gpu/dlss_bridge/third_party/nvngx-sdk ^
      -DVULKAN_INCLUDE=<Vulkan-Headers>/include
cmake --build out/dlss-bridge
```

The result is `out/dlss-bridge/bbport_dlss.dll`; `build.bat` copies it to `out/bbport_dlss.dll`
next to `bb-probe.exe`. Set `BB_DLSS_BRIDGE=0` to skip the step.

## Runtime files

DLSS needs three pieces, all optional:

| File | Where | Notes |
| --- | --- | --- |
| `bbport_dlss.dll` | next to `bb-probe.exe` (built by `build.bat`) | this bridge |
| `nvngx_dlss.dll` | next to `bb-probe.exe`, or in `<user>/dlss/` | NVIDIA's Super Resolution library, from a driver install (`C:\ProgramData\NVIDIA Corporation\NGX\models\...` and the DriverStore) or the SDK download |
| NVIDIA driver | — | RTX 20 series or newer for DLSS |

`<user>` is the port's user directory (`BB_GPU_USER_DIR`, i.e. the folder that holds saves and
shader caches); NGX also writes its logs there, in `dlss/`.

The loader order is: `BB_DLSS=0` → off; no `bbport_dlss.dll` → off; no `nvngx_dlss.dll` → off;
non-NVIDIA GPU → off; NVIDIA GPU/driver without DLSS support (GTX) → off. In every case the
upscaler silently stays on FSR 3.1 and the overlay shows why DLSS is unavailable.

## License

- Bridge source: MIT (`LICENSE.txt`).
- NVIDIA RTX SDKs license: `licenses/NVIDIA-DLSS-LICENSE.txt` (shipped with the port).
- `nvngx_dlss.dll` is NVIDIA's; do not add it to the repository or to release archives that are
  not covered by the NVIDIA RTX SDKs license.
