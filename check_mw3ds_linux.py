#!/usr/bin/env python3
"""
MW3DS simple dependency & path checker for Linux.

Run this from the MW3DS project folder (or give the path as argument):

    python3 check_mw3ds_linux.py
    python3 check_mw3ds_linux.py /path/to/mw3ds

It checks:
  - Python version
  - Required Python packages
  - devkitPro / 3DS toolchain
  - make, git
  - makerom
  - Morrowind folder (Data Files + ESMs)
  - Project structure (source, tools, Makefile)
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path

# ------------------------------------------------------------------
# Colours (works in most terminals)
# ------------------------------------------------------------------
GREEN  = "\033[92m"
YELLOW = "\033[93m"
RED    = "\033[91m"
CYAN   = "\033[96m"
BOLD   = "\033[1m"
RESET  = "\033[0m"

def ok(msg):   print(f"  {GREEN}✓{RESET} {msg}")
def warn(msg): print(f"  {YELLOW}!{RESET} {msg}")
def fail(msg): print(f"  {RED}✗{RESET} {msg}")
def info(msg): print(f"  {CYAN}→{RESET} {msg}")

# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
def which(cmd):
    return shutil.which(cmd)

def run(cmd, timeout=10):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return r.returncode == 0, (r.stdout + r.stderr).strip()
    except Exception as e:
        return False, str(e)

def check_python_packages(packages):
    missing = []
    for pkg in packages:
        # handle environment markers like "audioop-lts; python_version >= '3.13'"
        name = pkg.split(";")[0].strip()
        try:
            __import__(name.replace("-", "_") if name == "audioop-lts" else name)
            ok(f"Python package: {name}")
        except ImportError:
            # try common import names
            alt = name.replace("-", "_")
            try:
                __import__(alt)
                ok(f"Python package: {name}")
            except ImportError:
                fail(f"Python package missing: {name}")
                missing.append(name)
    return missing

# ------------------------------------------------------------------
# Main checks
# ------------------------------------------------------------------
def main():
    print(f"\n{BOLD}MW3DS Linux Checker{RESET}")
    print("=" * 50)

    # Project root
    if len(sys.argv) > 1:
        project = Path(sys.argv[1]).resolve()
    else:
        project = Path.cwd().resolve()

    print(f"\n{BOLD}1. Project folder{RESET}")
    info(f"Looking at: {project}")

    if not project.is_dir():
        fail("Folder does not exist")
        sys.exit(1)

    # Basic project files
    required_files = ["Makefile", "requirements.txt"]
    required_dirs  = ["source", "include", "tools"]

    good = True
    for f in required_files:
        if (project / f).is_file():
            ok(f"Found {f}")
        else:
            fail(f"Missing {f}")
            good = False

    for d in required_dirs:
        if (project / d).is_dir():
            ok(f"Found {d}/")
        else:
            fail(f"Missing {d}/")
            good = False

    if (project / "tools" / "convert").is_dir():
        ok("Found tools/convert/")
    else:
        warn("tools/convert/ not found (needed for data conversion)")

    if (project / "tools" / "build").is_dir():
        ok("Found tools/build/")
    else:
        warn("tools/build/ not found (needed for CIA packaging)")

    # ------------------------------------------------------------------
    print(f"\n{BOLD}2. Python{RESET}")
    py_ver = sys.version_info
    if py_ver >= (3, 10):
        ok(f"Python {py_ver.major}.{py_ver.minor}.{py_ver.micro}")
    else:
        fail(f"Python {py_ver.major}.{py_ver.minor} is too old (need 3.10+)")
        good = False

    # Packages from requirements.txt
    req_file = project / "requirements.txt"
    packages = ["numpy", "pillow", "miniaudio", "imageio-ffmpeg"]
    if req_file.is_file():
        with open(req_file) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    packages.append(line)

    # unique
    packages = list(dict.fromkeys(packages))
    missing_pkgs = check_python_packages(packages)

    if missing_pkgs:
        good = False
        print()
        info("Install missing packages with:")
        print(f"    pip install {' '.join(missing_pkgs)}")
        print("  or:")
        print(f"    pip install -r {req_file}")

    # ------------------------------------------------------------------
    print(f"\n{BOLD}3. Build tools{RESET}")

    if which("make"):
        ok("make found")
    else:
        fail("make not found (install build-essential)")
        good = False

    if which("git"):
        ok("git found")
    else:
        warn("git not found (only needed to clone/update the repo)")

    # devkitPro
    devkitpro = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    devkitarm = os.environ.get("DEVKITARM", os.path.join(devkitpro, "devkitARM"))

    if Path(devkitpro).is_dir():
        ok(f"DEVKITPRO = {devkitpro}")
    else:
        fail(f"DEVKITPRO not found at {devkitpro}")
        info("Install from https://devkitpro.org/wiki/Getting_Started")
        good = False

    gpp = which("arm-none-eabi-g++")
    if gpp:
        ok(f"arm-none-eabi-g++ → {gpp}")
    else:
        # try inside DEVKITARM
        candidate = Path(devkitarm) / "bin" / "arm-none-eabi-g++"
        if candidate.is_file():
            ok(f"arm-none-eabi-g++ → {candidate}")
            info("Add it to PATH: export PATH=\"$DEVKITARM/bin:$PATH\"")
        else:
            fail("arm-none-eabi-g++ not found")
            info("Run: sudo dkp-pacman -S 3ds-dev")
            good = False

    # makerom
    makerom = which("makerom")
    if not makerom:
        # look in project root
        local = project / "makerom"
        if local.is_file() and os.access(local, os.X_OK):
            makerom = str(local)
            ok(f"makerom found in project: {local}")
        else:
            fail("makerom not found")
            info("Place the makerom binary in the project folder or in your PATH")
            good = False
    else:
        ok(f"makerom → {makerom}")

    # ------------------------------------------------------------------
    print(f"\n{BOLD}4. Morrowind game folder{RESET}")

    # Try .env first
    morrowind = None
    env_file = project / ".env"
    if env_file.is_file():
        with open(env_file) as f:
            for line in f:
                line = line.strip()
                if line.startswith("MW3DS_MORROWIND_DIR="):
                    morrowind = line.split("=", 1)[1].strip().strip('"').strip("'")
                    break

    if not morrowind:
        morrowind = os.environ.get("MW3DS_MORROWIND_DIR")

    if not morrowind:
        # common locations
        candidates = [
            Path.home() / "games" / "Morrowind",
            Path.home() / "Games" / "Morrowind",
            Path.home() / ".local/share/Steam/steamapps/common/Morrowind",
            Path("/mnt/games/Morrowind"),
        ]
        for c in candidates:
            if c.is_dir() and (c / "Data Files").is_dir():
                morrowind = str(c)
                warn(f"No .env set – found possible install at {morrowind}")
                break

    if not morrowind:
        fail("Morrowind path not set")
        info("Create a .env file with:")
        print("    MW3DS_MORROWIND_DIR=/full/path/to/Morrowind")
        good = False
    else:
        mw = Path(morrowind)
        info(f"Using: {mw}")

        if not mw.is_dir():
            fail("Folder does not exist")
            good = False
        else:
            ok("Folder exists")

            data_files = mw / "Data Files"
            if data_files.is_dir():
                ok("Data Files/ found")
            else:
                fail("Data Files/ missing – is this the correct Morrowind folder?")
                good = False

            # Check for master files
            esms = ["Morrowind.esm"]
            optional = ["Tribunal.esm", "Bloodmoon.esm"]

            for esm in esms:
                if (data_files / esm).is_file():
                    ok(f"Found {esm}")
                else:
                    fail(f"Missing {esm}")
                    good = False

            for esm in optional:
                if (data_files / esm).is_file():
                    ok(f"Found {esm} (expansion)")
                else:
                    warn(f"{esm} not found (optional)")

    # ------------------------------------------------------------------
    print(f"\n{BOLD}5. Summary{RESET}")
    print("=" * 50)

    if good and not missing_pkgs:
        print(f"{GREEN}{BOLD}All important checks passed!{RESET}")
        print()
        print("You can now try a build. Simple commands:")
        print()
        print("  # 1. Make sure environment is set")
        print("  export DEVKITPRO=/opt/devkitpro")
        print("  export DEVKITARM=$DEVKITPRO/devkitARM")
        print("  export PATH=\"$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH\"")
        print()
        print("  # 2. (Optional) create/update .env")
        print(f"  echo 'MW3DS_MORROWIND_DIR={morrowind or \"/path/to/Morrowind\"}' > .env")
        print()
        print("  # 3. Build (if the project has the official build scripts)")
        print("  python tools/build/build_cia_mods.py")
        print("  # or for Old 3DS:")
        print("  python tools/build/build_cia_mods.py --old3ds")
        print()
        print("  # If you only have the older mw3ds_full.py:")
        print("  python mw3ds_full.py --morrowind-dir \"$MW3DS_MORROWIND_DIR\"")
        return 0
    else:
        print(f"{RED}{BOLD}Some things are missing.{RESET}")
        print("Fix the items marked with ✗ above, then run this script again.")
        return 1

if __name__ == "__main__":
    sys.exit(main())
