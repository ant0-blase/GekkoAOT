"""Check REL relocation provenance across loop and call control-flow edges."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / ".gekkoaot/src/DolRecomp/src"
BUILD = ROOT / ".gekkoaot/diagnostics/rel-cfg-address-tests"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / "check"
    subprocess.run([
        "cc", "-std=gnu11", "-O2", "-I", str(SOURCE),
        str(ROOT / "tests/hardware/rel_address_cfg/check.c"),
        str(SOURCE / "ir/dolir.c"), str(SOURCE / "ir/dolir_address.c"),
        str(SOURCE / "ir/dolir_effects.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
