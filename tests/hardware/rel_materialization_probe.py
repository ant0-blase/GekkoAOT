"""Compile and execute REL pointer materialization with both LLVM state modes."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".gekkoaot/diagnostics/rel-materialization-tests"


def main():
    subprocess.run([
        "cmake", "-S", str(ROOT / "tests/hardware/rel_materialization"),
        "-B", str(BUILD), "-G", "Ninja",
        f"-DDOLRECOMP_SOURCE={ROOT / '.gekkoaot/src/DolRecomp'}",
        "-DCMAKE_BUILD_TYPE=Release",
        f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR", "/usr/lib/llvm20")}/lib/cmake/llvm',
    ], check=True)
    subprocess.run([
        "cmake", "--build", str(BUILD), "--target",
        "rel_materialization_check_0", "rel_materialization_check_1", "-j4",
    ], check=True)
    for mode in (0, 1):
        subprocess.run([str(BUILD / f"rel_materialization_check_{mode}")], check=True)


if __name__ == "__main__":
    main()
