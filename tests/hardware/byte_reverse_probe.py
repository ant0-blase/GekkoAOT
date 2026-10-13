"""Compile and execute architectural byte-reverse instruction regressions."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / ".gekkoaot/src/DolRecomp"
BUILD = ROOT / ".gekkoaot/diagnostics/byte-reverse-tests"


def main():
    subprocess.run([
        "cmake", "-S", str(ROOT / "tests/hardware/byte_reverse"), "-B", str(BUILD),
        "-G", "Ninja", f"-DDOLRECOMP_SOURCE={SOURCE}", "-DCMAKE_BUILD_TYPE=Release",
        f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR", "/usr/lib/llvm20")}/lib/cmake/llvm',
    ], check=True)
    subprocess.run(["cmake", "--build", str(BUILD), "--target",
                    "byte_reverse_check_0", "byte_reverse_check_1", "-j4"], check=True)
    for mode in (0, 1):
        subprocess.run([str(BUILD / f"byte_reverse_check_{mode}")], check=True)


if __name__ == "__main__":
    main()
