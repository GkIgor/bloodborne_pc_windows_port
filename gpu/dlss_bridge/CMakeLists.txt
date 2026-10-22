# SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
# SPDX-License-Identifier: MIT
#
# bbport_dlss (bbport_dlss.dll) links the NVIDIA DLSS SDK and is built separately from the port.
# Everything else in bbport is MinGW-w64; this one DLL must be MSVC because NVIDIA's Windows NGX
# SDK ships MSVC-only libraries (nvsdk_ngx_s.lib uses the MSVC CRT and C++ EH metadata).
#
# The SDK's public part is vendored in third_party/nvngx-sdk (github.com/NVIDIA/DLSS):
#   include/nvsdk_ngx*.h  and  lib/Windows_x86_64/x64/nvsdk_ngx_s.lib
# Point DLSS_SDK_ROOT at another copy (e.g. .downloads/DLSS) to build against it instead.
#
# Build (from a "x64 Native Tools Command Prompt for VS", or after vcvars64.bat):
#   cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release ^
#         -DCMAKE_CXX_COMPILER=cl
#   cmake --build out/dlss-bridge
# build.bat does this automatically and copies the DLL next to bb-probe.exe.

cmake_minimum_required(VERSION 3.20)
project(bbport_dlss LANGUAGES CXX)

set(DEFAULT_DLSS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvngx-sdk")
set(DLSS_SDK_ROOT "${DEFAULT_DLSS_SDK_ROOT}" CACHE PATH
    "NVIDIA DLSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The DLSS bridge is x86-64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS)")
endif()
if (WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "NVIDIA's Windows NGX SDK ships MSVC-only libraries. Build gpu/dlss_bridge with MSVC "
        "(cl.exe); the rest of bbport stays MinGW-w64. DLSS falls back to FSR 3.1 without it.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nvsdk_ngx_s.lib is the static-CRT (/MT) variant: no Visual C++ runtime install needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(bbport_dlss SHARED bbport_dlss_bridge.cpp bbport_dlss_bridge.h)
target_include_directories(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/include" ${VULKAN_INCLUDE})

if (WIN32)
    # advapi32 (NGX reads its driver-store path from the registry) and user32 are not in MSVC's
    # default library set for a /LD build.
    target_link_libraries(bbport_dlss PRIVATE
        "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
        advapi32 user32)
    target_compile_definitions(bbport_dlss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # Only BbDlssGetApi is exported; NGX finds libnvidia-ngx-dlss.so.<version> next to bb-probe.
    set_target_properties(bbport_dlss PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_link_libraries(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a"
                                              ${CMAKE_DL_LIBS})
    target_link_options(bbport_dlss PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
endif()
