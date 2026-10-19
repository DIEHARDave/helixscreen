#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build gate: only reviewed objects call into the runtime-loaded GPU libraries.
#
# A DLOPEN_GPU_LIBS build (mk/implib.mk) reaches libdrm, libgbm, libEGL and
# libinput through trampolines, so the binary starts on a system without them.
# Every call path has to ask include/gpu_libs.h first; a path that does not
# gets 0 back from every function of a missing library, which is not a safe
# answer for all of them. This gate keeps the set of objects that reference
# any trampolined function to the list below, each entry checked by hand to
# sit behind a gpu_libs.h test, so a new caller fails the build until it is.
#
# Reads the function names out of the generated <lib>.init.c files and the
# undefined symbols of every object under the build directory.
#
# Usage:
#   scripts/check_gpu_lib_callers.py --build-dir build/pi32 [--nm arm-linux-gnueabihf-nm]

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

# Object (relative to the build dir, glob) -> where its calls are gated.
ALLOWED = {
    "obj/api/display_backend.o": "panel orientation and no-display diagnostics check Lib::Drm",
    "display/display_backend.o": "the same source, in the splash/watchdog display library",
    "obj/api/display_backend_drm.o": "constructor and is_available() check drm_libs_present()",
    "display/display_backend_drm.o": "the same source, in the splash/watchdog display library",
    "egl-variant/display_backend_drm.o": "the EGL compile; drm_libs_present() covers the GPU stack",
    "obj/lvgl/src/drivers/display/drm/lv_linux_drm.o": "reached only from DisplayBackendDRM",
    "egl-variant/lvgl/src/drivers/display/drm/lv_linux_drm_egl.o": "reached only from the EGL DisplayBackendDRM",
    "obj/lvgl/src/drivers/libinput/lv_libinput.o": "reached only behind Lib::Input in DisplayBackendDRM",
    "obj/application/probe_egl_cmd.o": "run_probe_egl() checks every library before touching one",
    "obj/rendering/gcode_gles_renderer.o": "init_gl() checks Lib::Egl/Gles2, the GBM path Lib::Gbm/Drm",
    "obj/ui/backdrop_blur.o": "gpu_blur() checks all four before the first EGL call",
}

SYM_RE = re.compile(r'^\s*"([A-Za-z_][A-Za-z0-9_]*)",\s*$')


def stubbed_functions(build_dir: Path) -> set[str]:
    names: set[str] = set()
    for init_c in sorted((build_dir / "implib").glob("*.init.c")):
        for line in init_c.read_text().splitlines():
            m = SYM_RE.match(line)
            if m:
                names.add(m.group(1))
    return names


def undefined_symbols(nm: str, objects: list[Path]) -> dict[Path, set[str]]:
    result: dict[Path, set[str]] = {}
    for i in range(0, len(objects), 200):
        batch = objects[i:i + 200]
        out = subprocess.run([nm, "-A", "-u", *map(str, batch)], capture_output=True, text=True)
        for line in out.stdout.splitlines():
            path, _, rest = line.partition(":")
            fields = rest.split()
            if fields:
                result.setdefault(Path(path), set()).add(fields[-1].split("@")[0])
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--nm", default="nm")
    args = ap.parse_args()

    build_dir = args.build_dir
    stubbed = stubbed_functions(build_dir)
    if not stubbed:
        print(f"gpu-lib callers: no trampoline tables under {build_dir}/implib", file=sys.stderr)
        return 1

    objects = sorted(p for p in build_dir.rglob("*.o") if "implib" not in p.relative_to(build_dir).parts)
    callers: dict[str, list[str]] = {}
    for obj, syms in undefined_symbols(args.nm, objects).items():
        used = sorted(syms & stubbed)
        if used:
            callers[obj.relative_to(build_dir).as_posix()] = used

    unreviewed = {o: s for o, s in callers.items()
                  if not any(fnmatch.fnmatch(o, pat) for pat in ALLOWED)}
    if unreviewed:
        print("gpu-lib callers: objects call into a runtime-loaded GPU library without review:",
              file=sys.stderr)
        for obj, syms in sorted(unreviewed.items()):
            print(f"  {obj}: {', '.join(syms[:6])}{' ...' if len(syms) > 6 else ''}", file=sys.stderr)
        print("Gate each call path on include/gpu_libs.h, then add the object to ALLOWED in\n"
              "scripts/check_gpu_lib_callers.py with where it is gated.", file=sys.stderr)
        return 1

    print(f"gpu-lib callers: {len(callers)} reviewed objects call into "
          f"{len(set().union(*callers.values()))} of {len(stubbed)} stubbed functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
