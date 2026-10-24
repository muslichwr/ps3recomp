#!/usr/bin/env python3
"""Real generated GCM NIDs -> production PPU bridge -> real GCM return values.

Uses the existing test_hle_integration.cpp fixture and GNU-driver compilers.
On Windows, explicitly set CC/CXX to a MinGW pair (e.g. CLANG64 clang/clang++);
the MSVC/clang-cl standalone link configuration is not supported here.
--build-dir preserves task-local outputs, otherwise uses disposable test scratch.
"""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", type=Path)
    args = ap.parse_args()
    if os.name == "nt" and not (os.environ.get("CC") or os.environ.get("CXX")):
        print("SKIP: GCM HLE fixture needs explicit MinGW CC/CXX on Windows; clang-cl is unsupported")
        return 0
    if os.name == "nt" and not (os.environ.get("CC") and os.environ.get("CXX")):
        raise SystemExit("FAIL: configure both MinGW CC and CXX on Windows")
    cc = shutil.which(os.environ.get("CC", "cc"))
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if not cc or not cxx:
        raise SystemExit("FAIL: C and C++ compilers are required")
    if os.name == "nt":
        for compiler in (cc, cxx):
            target = subprocess.run([compiler, "-dumpmachine"], capture_output=True,
                                    text=True, check=True, timeout=10).stdout.lower()
            if "mingw" not in target and "windows-gnu" not in target:
                raise SystemExit("FAIL: configured Windows compilers must target MinGW")
    if args.build_dir:
        args.build_dir.mkdir(parents=True, exist_ok=True)
    workspace = (nullcontext(str(args.build_dir.resolve())) if args.build_dir else
                 tempfile.TemporaryDirectory(prefix="ps3-gcm-hle-"))
    with workspace as directory:
        tmp = Path(directory)
        build_env = dict(os.environ, TEMP=str(tmp), TMP=str(tmp), TMPDIR=str(tmp))
        (tmp / "ppu_recomp.h").write_text(
            '#include "ppu_context.h"\nextern "C" {\n'
            'extern uint8_t* vm_base;\nuint32_t vm_read32(uint64_t);\n'
            'uint64_t vm_read64(uint64_t);\nvoid vm_write32(uint64_t,uint32_t);\n'
            'void vm_write64(uint64_t,uint64_t);\n}\n', encoding="utf-8")
        includes = ["-I" + str(p) for p in
                    (tmp, ROOT / "include", ROOT / "runtime/ppu", ROOT / "runtime/syscalls")]
        common = ["-O1", "-UNDEBUG", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", *includes]
        subprocess.run([sys.executable, "-B", str(ROOT / "tools/gen_hle_nids.py"),
                        "cellGcmSys", "--out", str(tmp / "nids.cpp")],
                       env=build_env, check=True, timeout=30)
        objects = []
        for source in ("cellGcmSys", "rsx_commands"):
            obj = tmp / (source + ".o")
            subprocess.run([cc, "-std=gnu17", *common, "-c",
                            str(ROOT / f"libs/video/{source}.c"), "-o", str(obj)],
                           env=build_env, check=True, timeout=60)
            objects.append(str(obj))
        exe = tmp / ("test_hle_integration.exe" if os.name == "nt" else "test_hle_integration")
        gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        subprocess.run([cxx, "-std=gnu++17", *common,
                        str(ROOT / "runtime/ppu/tests/test_hle_integration.cpp"),
                        str(ROOT / "runtime/ppu/ppu_hle.cpp"), str(tmp / "nids.cpp"),
                        *objects, gc, "-pthread", "-o", str(exe)],
                       env=build_env, check=True, timeout=90)
        env = {k: v for k, v in os.environ.items()
               if k.upper() in ("PATH", "SYSTEMROOT", "WINDIR", "SYSTEMDRIVE", "COMSPEC")}
        env.update(TEMP=str(tmp), TMP=str(tmp), TMPDIR=str(tmp))
        return subprocess.run([str(exe)], cwd=tmp, env=env, timeout=20).returncode


if __name__ == "__main__":
    raise SystemExit(main())
