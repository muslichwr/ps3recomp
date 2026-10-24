#!/usr/bin/env python3
"""Real generic/context HLE dispatch must resize context-open descriptors.

Usage: python tools/test_ppu_fs_ftruncate.py [--temp-root PATH]
Uses CC/CXX, a generated cellFs NID table and original synthetic bytes only.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--temp-root", type=Path)
    args = ap.parse_args()
    cc = shutil.which(os.environ.get("CC", "cc"))
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if not cc or not cxx:
        print("FAIL: C and C++ compilers are required")
        return 1
    with tempfile.TemporaryDirectory(prefix="ps3-ftruncate-", dir=args.temp_root) as tmp:
        tmp = Path(tmp)
        (tmp / "ppu_recomp.h").write_text(
            '#include "ppu_context.h"\nextern "C" {\n'
            'extern uint8_t* vm_base;\nuint32_t vm_read32(uint64_t);\n'
            'uint64_t vm_read64(uint64_t);\nvoid vm_write32(uint64_t,uint32_t);\n'
            'void vm_write64(uint64_t,uint64_t);\n}\n', encoding="utf-8")
        includes = ["-I" + str(p) for p in (tmp, ROOT / "include", ROOT / "runtime/ppu", ROOT / "runtime/syscalls")]
        common = ["-O1", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", *includes]
        subprocess.run([sys.executable, "-B", str(ROOT / "tools/gen_hle_nids.py"),
                        "cellFs", "--out", str(tmp / "nids.cpp")], check=True, timeout=30)
        for name in ("cellFs", "edat"):
            subprocess.run([cc, "-std=gnu17", *common, "-c", str(ROOT / f"libs/filesystem/{name}.c"),
                            "-o", str(tmp / f"{name}.o")], check=True, timeout=60)
        exe = tmp / ("test_fs_ftruncate.exe" if os.name == "nt" else "test_fs_ftruncate")
        gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        subprocess.run([cxx, "-std=gnu++17", *common,
                        str(ROOT / "runtime/ppu/tests/test_fs_ftruncate.cpp"), str(tmp / "nids.cpp"),
                        str(tmp / "cellFs.o"), str(tmp / "edat.o"), gc, "-o", str(exe)],
                       check=True, timeout=90)
        env = {k: v for k, v in os.environ.items()
               if k.upper() in ("PATH", "SYSTEMROOT", "WINDIR", "SYSTEMDRIVE", "COMSPEC")}
        env.update(PS3_HDD0_ROOT=str(tmp), TEMP=str(tmp), TMP=str(tmp))
        (tmp / "contract.bin").write_bytes(bytes([0xA5]) * 8192)
        return subprocess.run([str(exe), str(tmp / "contract.bin")], env=env,
                              cwd=tmp, timeout=20).returncode


if __name__ == "__main__":
    raise SystemExit(main())
