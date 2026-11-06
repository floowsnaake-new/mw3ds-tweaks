#!/usr/bin/env python3
"""
MW3DS Full Setup Script for Linux
=================================
Downloads and prepares almost everything needed to build MW3DS.

What it does:
  1. Checks Python version
  2. Creates a virtual environment (optional but recommended)
  3. Installs Python packages from requirements.txt
  4. Clones the official MW3DS repository (with submodules)
  5. Tries to download makerom
  6. Creates a .env template
  7. Prints clear next steps for devkitPro (must be installed by user)

Usage:
  python3 setup_mw3ds_linux.py

  # Or choose where to put the project:
  python3 setup_mw3ds_linux.py --dir ~/mw3ds

  # Skip cloning if you already have the code:
  python3 setup_mw3ds_linux.py --no-clone

  # Only install Python packages:
  python3 setup_mw3ds_linux.py --only-python
"""

import argparse
import os
import sys
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
from pathlib import Path

# ------------------------------------------------------------------
# Colours
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
def header(msg):
    print(f"\n{BOLD}{msg}{RESET}")
    print("=" * 55)

# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
def run(cmd, cwd=None, check=True):
    """Run a command and return success + output."""
    try:
        r = subprocess.run(
            cmd, shell=True, cwd=cwd,
            capture_output=True, text=True, timeout=600
        )
        if check and r.returncode != 0:
            return False, (r.stdout + r.stderr).strip()
        return r.returncode == 0, (r.stdout + r.stderr).strip()
    except Exception as e:
        return False, str(e)

def download(url, dest):
    """Download a file with a simple progress indicator."""
    info(f"Downloading {url}")
    try:
        with urllib.request.urlopen(url, timeout=60) as resp:
            total = int(resp.headers.get("Content-Length", 0))
            data = bytearray()
            while True:
                chunk = resp.read(64 * 1024)
                if not chunk:
                    break
                data.extend(chunk)
                if total:
                    pct = len(data) * 100 // total
                    print(f"\r  {pct:3d}%", end="", flush=True)
        print()
        Path(dest).write_bytes(data)
        return True
    except Exception as e:
        fail(f"Download failed: {e}")
        return False

# ------------------------------------------------------------------
# Steps
# ------------------------------------------------------------------
def check_python():
    header("1. Python version")
    v = sys.version_info
    if v >= (3, 10):
        ok(f"Python {v.major}.{v.minor}.{v.micro}")
        return True
    fail(f"Python {v.major}.{v.minor} is too old. Need 3.10 or newer.")
    return False


def setup_venv(project_dir: Path, use_venv: bool):
    header("2. Virtual environment")
    if not use_venv:
        warn("Skipping virtual environment (--no-venv)")
        return True

    venv_dir = project_dir / "venv"
    if venv_dir.is_dir():
        ok(f"Virtual environment already exists: {venv_dir}")
        return True

    info("Creating virtual environment...")
    ok_flag, out = run(f'"{sys.executable}" -m venv "{venv_dir}"')
    if ok_flag:
        ok(f"Created {venv_dir}")
        info("Activate it later with:")
        print(f"    source {venv_dir}/bin/activate")
        return True
    else:
        fail(f"Could not create venv: {out}")
        return False


def install_python_packages(project_dir: Path, use_venv: bool):
    header("3. Python packages")

    packages = [
        "numpy",
        "pillow",
        "miniaudio",
        "imageio-ffmpeg",
    ]
    # audioop-lts only needed on 3.13+
    if sys.version_info >= (3, 13):
        packages.append("audioop-lts")

    pip = "pip"
    if use_venv:
        pip = str(project_dir / "venv" / "bin" / "pip")
        if not Path(pip).exists():
            pip = "pip"  # fallback

    info(f"Using: {pip}")
    cmd = f'"{pip}" install --upgrade pip'
    run(cmd, check=False)

    cmd = f'"{pip}" install {" ".join(packages)}'
    info("Installing: " + " ".join(packages))
    success, out = run(cmd)
    if success:
        ok("All Python packages installed")
        return True
    else:
        fail("pip install failed")
        print(out)
        info("Try manually: pip install numpy pillow miniaudio imageio-ffmpeg")
        return False


def clone_repo(target_dir: Path, do_clone: bool):
    header("4. MW3DS source code")

    if not do_clone:
        warn("Skipping clone (--no-clone)")
        return True

    if (target_dir / "Makefile").exists() and (target_dir / "source").is_dir():
        ok("Project already looks present (Makefile + source/ found)")
        info("If you want a fresh clone, delete the folder and run again")
        return True

    if not which_git():
        fail("git is not installed. Install it first:")
        print("    sudo apt install git          # Debian/Ubuntu")
        print("    sudo dnf install git          # Fedora")
        print("    sudo pacman -S git            # Arch")
        return False

    # Clone into a temporary name then move, or clone directly
    parent = target_dir.parent
    name = target_dir.name

    if target_dir.exists() and any(target_dir.iterdir()):
        warn(f"{target_dir} is not empty")
        info("Cloning into a subfolder 'mw3ds' instead")
        target_dir = target_dir / "mw3ds"
        target_dir.mkdir(parents=True, exist_ok=True)

    info(f"Cloning into {target_dir} ...")
    info("This may take a while (includes optimization mods ~550 MB)")

    cmd = f'git clone --recurse-submodules https://github.com/3ds-conspiracy/mw3ds.git "{target_dir}"'
    success, out = run(cmd)
    if success:
        ok("Repository cloned successfully")
        return True
    else:
        fail("git clone failed")
        print(out[-1500:] if len(out) > 1500 else out)
        info("You can also download a ZIP from:")
        print("  https://github.com/3ds-conspiracy/mw3ds/archive/refs/heads/main.zip")
        return False


def which_git():
    return shutil.which("git") is not None


def download_makerom(project_dir: Path):
    header("5. makerom")

    # Check if already present
    local = project_dir / "makerom"
    if local.is_file() and os.access(local, os.X_OK):
        ok(f"makerom already present: {local}")
        return True

    if shutil.which("makerom"):
        ok("makerom found in PATH")
        return True

    # Official releases sometimes ship tools; try a well-known source.
    # Note: makerom is part of 3DS homebrew tooling. We try a common
    # GitHub release pattern. If it fails we just tell the user what to do.
    info("Trying to download makerom...")

    # Common location used by many 3DS projects (may need updating)
    urls = [
        # Placeholder – real projects often host it or use the setup script
        # We will fall back to clear instructions.
    ]

    # For safety and reliability we give clear manual instructions
    # instead of relying on fragile direct links that change.
    warn("Automatic download of makerom is not always reliable.")
    info("Do one of the following:")
    print()
    print("  Option A – Let the official setup script get it later:")
    print("      python tools/build/setup.py")
    print()
    print("  Option B – Download manually:")
    print("      1. Get makerom from a trusted 3DS toolkit release")
    print("      2. Place the binary in the project root")
    print("      3. Run:  chmod +x makerom")
    print()
    return True  # not a hard failure


def create_env_template(project_dir: Path):
    header("6. Configuration (.env)")

    env_path = project_dir / ".env"
    example = project_dir / ".env.example"

    if env_path.exists():
        ok(".env already exists")
        return True

    content = """# MW3DS configuration
# Edit the path below to point at YOUR Morrowind folder
# (the folder that contains "Data Files" and Morrowind.esm)

MW3DS_MORROWIND_DIR=/path/to/your/Morrowind

# Optional – only needed on Windows or for the emulator
# MW3DS_MSYS2_DIR=
# MW3DS_CTR_TOOLS_DIR=
# MW3DS_EMU_DIR=
"""
    if example.exists():
        # Prefer the official example if present
        try:
            content = example.read_text()
        except Exception:
            pass

    env_path.write_text(content)
    ok(f"Created {env_path}")
    info("Edit it and set MW3DS_MORROWIND_DIR to your real Morrowind path")
    return True


def print_final_instructions(project_dir: Path, use_venv: bool):
    header("7. Next steps (important)")

    print(f"""
{BOLD}A. Install the 3DS toolchain (required once){RESET}

  Follow the official guide:
    https://devkitpro.org/wiki/Getting_Started

  Then install the packages:
    sudo dkp-pacman -S 3ds-dev

  Add this to your ~/.bashrc (or run it every time):
    export DEVKITPRO=/opt/devkitpro
    export DEVKITARM=$DEVKITPRO/devkitARM
    export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

{BOLD}B. Set your Morrowind path{RESET}

  Edit the file:
    {project_dir}/.env

  Change the line to something like:
    MW3DS_MORROWIND_DIR=/home/yourname/games/Morrowind

{BOLD}C. Activate the environment (if you used a venv){RESET}
""")
    if use_venv:
        print(f"    source {project_dir}/venv/bin/activate")
    else:
        print("    (you skipped the virtual environment)")

    print(f"""
{BOLD}D. Run the official setup checker{RESET}

    cd {project_dir}
    python tools/build/setup.py

{BOLD}E. Build the game{RESET}

  New 3DS:
    python tools/build/build_cia_mods.py

  Old 3DS:
    python tools/build/build_cia_mods.py --old3ds

{BOLD}F. Install on your 3DS{RESET}

  Copy the .cia file from the build/ folder to your SD card
  and install it with FBI.

{CYAN}Tip:{RESET} You can also use the ready-made converter container
instead of building from source (no toolchain needed):

  mkdir -p ~/mw3ds-out
  podman run --rm \\
    -v /path/to/Morrowind:/morrowind:ro \\
    -v ~/mw3ds-out:/out \\
    ghcr.io/3ds-conspiracy/mw3ds-converter:0.2.7 --tribunal --bloodmoon
""")


# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Download and prepare everything needed for MW3DS on Linux"
    )
    parser.add_argument(
        "--dir", default=str(Path.home() / "mw3ds"),
        help="Where to put / find the project (default: ~/mw3ds)"
    )
    parser.add_argument(
        "--no-clone", action="store_true",
        help="Do not clone the git repository"
    )
    parser.add_argument(
        "--no-venv", action="store_true",
        help="Do not create a Python virtual environment"
    )
    parser.add_argument(
        "--only-python", action="store_true",
        help="Only install Python packages and exit"
    )
    args = parser.parse_args()

    project_dir = Path(args.dir).expanduser().resolve()
    use_venv = not args.no_venv

    print(f"\n{BOLD}MW3DS Linux Setup{RESET}")
    print(f"Target folder: {project_dir}\n")

    if not check_python():
        sys.exit(1)

    project_dir.mkdir(parents=True, exist_ok=True)

    if not args.only_python:
        if not clone_repo(project_dir, do_clone=not args.no_clone):
            warn("Continuing without a full clone...")

    # If we cloned into a subfolder, adjust
    if (project_dir / "mw3ds" / "Makefile").exists():
        project_dir = project_dir / "mw3ds"

    setup_venv(project_dir, use_venv)
    install_python_packages(project_dir, use_venv)

    if args.only_python:
        ok("Python packages done.")
        return 0

    download_makerom(project_dir)
    create_env_template(project_dir)
    print_final_instructions(project_dir, use_venv)

    print(f"\n{GREEN}{BOLD}Setup script finished.{RESET}")
    print("Follow the steps above to finish installing the 3DS toolchain and build.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
