#!/usr/bin/env python3
"""
Downloads the FSR 4 v07 INT8/DOT4 SPIR-V asset set used by the Vulkan upscaler provider into fsr4_shaders/.
Assets originate from AMD's MIT-licensed FidelityFX SDK FSR 4 implementation (compiled for Vulkan by Q2RTX).

License: MIT (see LICENSE-FSR4-v07.txt)
Upstream Commit: ae8d628fae208813172446d1e49ed94150b04658
"""

import argparse
import concurrent.futures
import os
import sys
import urllib.request
from pathlib import Path

COMMIT = "ae8d628fae208813172446d1e49ed94150b04658"
BASE_URL = f"https://raw.githubusercontent.com/FireBurn/Q2RTX/{COMMIT}/baseq2/fsr4_shaders"

MODELS = ["native", "quality", "balanced", "performance", "ultraperf", "drs"]
DEFAULT_TIERS = ["1080", "2160"]


def build_file_list(tiers):
    files = ["LICENSE-FSR4-v07.txt", "rcas.spv", "spd_auto_exposure.spv"]
    for model in MODELS:
        files.extend([
            f"fsr4_model_v07_i8_{model}_initializers.bin",
            f"fsr4_model_v07_i8_{model}_pre_weights.bin",
            f"fsr4_model_v07_i8_{model}_shader_manifest.json",
        ])
        for tier in tiers:
            files.extend([
                f"fsr4_model_v07_i8_{model}_{tier}_pre.spv",
                f"fsr4_model_v07_i8_{model}_{tier}_post.spv",
            ])
            for p in range(1, 13):
                files.append(f"fsr4_model_v07_i8_{model}_{tier}_pass{p}.spv")
    return files


def download_one(filename, dest_dir, base_url):
    target = dest_dir / filename
    if target.exists() and target.stat().st_size > 0:
        return filename, True, "exists"
    url = f"{base_url}/{filename}"
    part = dest_dir / f"{filename}.part"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "BloodbornePC-AssetFetcher/1.0"})
        with urllib.request.urlopen(req, timeout=30) as resp, open(part, "wb") as f:
            f.write(resp.read())
        part.replace(target)
        return filename, True, "downloaded"
    except Exception as e:
        if part.exists():
            part.unlink(missing_ok=True)
        return filename, False, str(e)


def main():
    parser = argparse.ArgumentParser(description="Fetch FSR 4 Vulkan shaders and weights.")
    parser.add_argument("--dest", type=Path, default=Path(__file__).resolve().parent.parent / "fsr4_shaders",
                        help="Target directory for downloaded shaders (default: fsr4_shaders in repo root)")
    parser.add_argument("--tiers", nargs="+", default=DEFAULT_TIERS,
                        help="Resolution tiers to download (default: 1080 2160)")
    parser.add_argument("--workers", type=int, default=8,
                        help="Parallel download workers (default: 8)")
    args = parser.parse_args()

    args.dest.mkdir(parents=True, exist_ok=True)
    file_list = build_file_list(args.tiers)
    total = len(file_list)
    print(f"Fetching FSR 4 assets into: {args.dest}")
    print(f"Total files to check: {total} ({len(MODELS)} presets, tiers: {', '.join(args.tiers)})")

    downloaded = 0
    skipped = 0
    failed = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        future_map = {executor.submit(download_one, fn, args.dest, BASE_URL): fn for fn in file_list}
        completed = 0
        for future in concurrent.futures.as_completed(future_map):
            completed += 1
            fn, ok, status = future.result()
            if ok:
                if status == "downloaded":
                    downloaded += 1
                else:
                    skipped += 1
            else:
                failed.append((fn, status))
            sys.stdout.write(f"\rProgress: [{completed}/{total}] downloaded: {downloaded}, cached: {skipped}, failed: {len(failed)}")
            sys.stdout.flush()

    print("\n")
    if failed:
        print(f"Warning: {len(failed)} files failed to download:")
        for fn, err in failed[:10]:
            print(f"  - {fn}: {err}")
        if len(failed) > 10:
            print(f"  ... and {len(failed) - 10} more.")
        sys.exit(1)
    else:
        print(f"Success! All {total} FSR 4 assets are present in {args.dest}")
        print("Licensing notice preserved in fsr4_shaders/LICENSE-FSR4-v07.txt.")


if __name__ == "__main__":
    main()
