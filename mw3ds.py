#!/usr/bin/env python3
"""
mw3ds.py - MW3DS launcher (single file, cross-platform).

On first run it asks for the Morrowind install root and saves it in
mw3ds.cfg next to this script. Every run then:

  1. checks every dependency the pipeline needs
  2. offers to fix anything fixable (venv, pip, converter patch)
  3. shows a task menu:
        1) Convert game data only
        2) Build the CIA only (needs out/data)
        3) Convert data + build the CIA
        4) Re-run the dependency check
        5) Verify the result
        6) Restore converter originals
        0) Quit

Everything (convert, compile, package) runs inside this Python file - no
external build scripts needed.

makerom lookup is strict per OS:
    Linux   -> Linux_makerom
    macOS   -> mac_makerom
    Windows -> Windows_makerom.exe

Usage:
    python3 mw3ds.py
    python3 mw3ds.py --task all
    python3 mw3ds.py --task all --morrowind-dir ~/games/Morrowind
    python3 mw3ds.py --check-only
    python3 mw3ds.py --reset           # forget the saved Morrowind path
"""
import argparse
import configparser
import gc
import glob
import os
import platform
import re
import shutil
import stat
import subprocess
import sys

IS_WINDOWS = platform.system() == "Windows"
IS_MAC     = platform.system() == "Darwin"
IS_LINUX   = platform.system() == "Linux"

HERE     = os.path.dirname(os.path.abspath(__file__))
CFG      = os.path.join(HERE, "mw3ds.cfg")
BAK      = ".pre-expansions.bak"
MAX_JOBS = 4

# The only makerom filename each OS is allowed to use.
MAKEROM_NAMES = {
    "linux":   "Linux_makerom",
    "mac":     "mac_makerom",
    "windows": "Windows_makerom.exe",
}


# ==========================================================================
# small utilities
# ==========================================================================
def _chmod_x(path):
    if IS_WINDOWS:
        return
    try:
        m = os.stat(path).st_mode
        if not (m & stat.S_IXUSR):
            os.chmod(path, m | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass


def _venv_python(root):
    if IS_WINDOWS:
        return os.path.join(root, ".venv", "Scripts", "python.exe")
    return os.path.join(root, ".venv", "bin", "python")


def _default_devkitpro():
    if IS_WINDOWS:
        return r"C:\devkitPro"
    for c in ("/opt/devkitpro", os.path.expanduser("~/devkitpro")):
        if os.path.isdir(os.path.join(c, "devkitARM")):
            return c
    return "/opt/devkitpro"


def _rmtree(path):
    if os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)


def _run(cmd, cwd, env=None, label=None):
    print("  $ " + (label or " ".join(cmd)))
    r = subprocess.run(cmd, cwd=cwd, env=env)
    if r.returncode != 0:
        sys.exit("  step failed: %s (exit %d)" % (cmd[0], r.returncode))


def _devkit_env():
    devkit = _default_devkitpro()
    if not os.path.isfile(os.path.join(devkit, "devkitARM", "3ds_rules")):
        sys.exit("  devkitPro not found at %s - install from "
                 "https://devkitpro.org/wiki/Getting_Started" % devkit)
    env = dict(os.environ)
    env["DEVKITPRO"] = devkit
    env["DEVKITARM"] = os.path.join(devkit, "devkitARM")
    extra = [os.path.join(devkit, "tools", "bin"),
             os.path.join(devkit, "devkitARM", "bin")]
    if IS_WINDOWS:
        msys = os.path.join(devkit, "msys2", "usr", "bin")
        if os.path.isdir(msys):
            extra.insert(0, msys)
    env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    return env


def _copy_tree(src, dst):
    total = sum(len(f) for _, _, f in os.walk(src))
    n = 0
    os.makedirs(dst, exist_ok=True)
    for root_, _dirs, files in os.walk(src):
        rel = os.path.relpath(root_, src)
        target = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root_, f), os.path.join(target, f))
            n += 1
            if n % 500 == 0:
                sys.stdout.write("\r  copied %d/%d" % (n, total)); sys.stdout.flush()
    sys.stdout.write("\r  copied %d/%d   \n" % (n, total)); sys.stdout.flush()


# ==========================================================================
# config (Morrowind path remembered between runs)
# ==========================================================================
def load_cfg():
    cp = configparser.ConfigParser()
    if os.path.isfile(CFG):
        try:
            cp.read(CFG)
        except configparser.Error:
            pass
    if not cp.has_section("mw3ds"):
        cp.add_section("mw3ds")
    return cp


def save_cfg(cp):
    if not cp.has_section("mw3ds"):
        cp.add_section("mw3ds")
    with open(CFG, "w") as f:
        cp.write(f)


def valid_morrowind(path):
    return bool(path) and os.path.isdir(os.path.join(path, "Data Files"))


def ask_morrowind(cfg, from_arg):
    if not cfg.has_section("mw3ds"):
        cfg.add_section("mw3ds")

    if from_arg:
        if not valid_morrowind(from_arg):
            sys.exit("ERROR: --morrowind-dir must contain a 'Data Files' folder.")
        cfg["mw3ds"]["morrowind_dir"] = from_arg
        save_cfg(cfg)
        return from_arg

    saved = cfg.get("mw3ds", "morrowind_dir", fallback="").strip()
    if saved and valid_morrowind(saved):
        return saved

    candidates = []
    if IS_WINDOWS:
        candidates += [r"C:\Games\Morrowind",
                       r"C:\Program Files (x86)\Bethesda Softworks\Morrowind",
                       r"C:\Program Files\Bethesda Softworks\Morrowind"]
    else:
        candidates += [
            os.path.expanduser("~/games/Morrowind"),
            os.path.expanduser("~/.wine/drive_c/Games/Morrowind"),
            "/mnt/c/Games/Morrowind",
        ]
    for c in candidates:
        if valid_morrowind(c):
            print("  found Morrowind at: " + c)
            cfg["mw3ds"]["morrowind_dir"] = c
            save_cfg(cfg)
            return c

    print()
    print("  No Morrowind install found automatically.")
    print("  Please enter the Morrowind install ROOT (the folder that CONTAINS 'Data Files').")
    while True:
        try:
            path = input("  Morrowind path: ").strip().strip('"').strip("'")
        except (EOFError, KeyboardInterrupt):
            sys.exit("\n  aborted")
        if not path:
            continue
        if valid_morrowind(path):
            cfg["mw3ds"]["morrowind_dir"] = path
            save_cfg(cfg)
            print("  saved to mw3ds.cfg")
            return path
        print("  That folder has no 'Data Files' subfolder. Try again.")


# ==========================================================================
# makerom lookup (strict per OS)
# ==========================================================================
def _makerom_name():
    if IS_WINDOWS:
        return MAKEROM_NAMES["windows"]
    if IS_MAC:
        return MAKEROM_NAMES["mac"]
    return MAKEROM_NAMES["linux"]


def find_makerom(root):
    """Return the path of the makerom for THIS OS, or None.
    Strict: on Linux only Linux_makerom counts, on macOS only mac_makerom,
    on Windows only Windows_makerom.exe. No cross-platform fallbacks."""
    p = os.path.join(root, _makerom_name())
    return p if os.path.isfile(p) else None


# ==========================================================================
# dependency checks
# ==========================================================================
_IMPORT_NAMES = {"pillow": "PIL", "audioop-lts": "audioop"}


def required_python_modules(root):
    names = []
    reqs = os.path.join(root, "requirements.txt")
    if os.path.isfile(reqs):
        with open(reqs, encoding="utf-8") as f:
            for line in f:
                line = line.split("#")[0].strip()
                m = re.match(r"[A-Za-z0-9_.\-]+", line)
                if m:
                    pkg = m.group(0).lower()
                    names.append(_IMPORT_NAMES.get(pkg, pkg.replace("-", "_")))
    return names or ["audioop", "numpy", "PIL", "miniaudio"]


def _has_modules(py, names):
    probe = "import " + ", ".join(names)
    try:
        return subprocess.run([py, "-c", probe], capture_output=True,
                              timeout=20).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def python_ok(root):
    needs = required_python_modules(root)
    if _has_modules(sys.executable, needs):
        return sys.executable, []
    venv = _venv_python(root)
    if os.path.isfile(venv) and _has_modules(venv, needs):
        return venv, []
    missing = [m for m in needs if not _has_modules(sys.executable, [m])]
    return None, missing


def check_all(root, mw):
    problems, fixable = [], []

    print("=" * 62)
    print(" Dependency check")
    print("=" * 62)
    print("  OS          : %s %s" % (platform.system(), platform.release()))
    print("  Python      : %s" % sys.version.split()[0])
    print("  Project     : %s" % root)
    print("  Morrowind   : %s" % mw)
    print()

    print("--- converter source ---")
    for rel in ("tools/convert/mwfiles.py",
                "tools/convert/level.py",
                "tools/convert/build_game.py",
                "Makefile", "requirements.txt"):
        if os.path.isfile(os.path.join(root, rel)):
            print("  OK      " + rel)
        else:
            print("  MISSING " + rel)
            problems.append("missing " + rel)

    print("--- python modules ---")
    py, missing = python_ok(root)
    if py:
        print("  OK      all imports available (%s)" % py)
    else:
        print("  MISSING " + ", ".join(missing))
        fixable.append("python")

    print("--- devkitPro ---")
    devkit = _default_devkitpro()
    if os.path.isfile(os.path.join(devkit, "devkitARM", "3ds_rules")):
        print("  OK      " + devkit)
        os.environ.setdefault("DEVKITPRO", devkit)
        os.environ.setdefault("DEVKITARM", os.path.join(devkit, "devkitARM"))
    else:
        print("  MISSING devkitPro at " + devkit)
        problems.append("devkitPro not installed (https://devkitpro.org/wiki/Getting_Started)")

    print("--- build tools ---")
    make_bin = shutil.which("make.exe" if IS_WINDOWS else "make") or shutil.which("make")
    if make_bin:
        print("  OK      make -> " + make_bin)
    else:
        print("  MISSING make on PATH")
        problems.append("make not on PATH")

    print("--- makerom ---")
    expected = _makerom_name()
    mk = find_makerom(root)
    if mk:
        print("  OK      " + os.path.basename(mk))
        _chmod_x(mk)
    else:
        print("  MISSING " + expected)
        problems.append("missing " + expected + " in " + root)

    print("--- build assets ---")
    rsf = glob.glob(os.path.join(root, "tools", "build", "*.rsf"))
    bnr = glob.glob(os.path.join(root, "tools", "build", "*.bnr"))
    if rsf: print("  OK      rsf: " + os.path.basename(rsf[0]))
    else:
        print("  MISSING .rsf"); problems.append("no .rsf in tools/build/")
    if bnr: print("  OK      bnr: " + os.path.basename(bnr[0]))
    else:
        print("  MISSING .bnr"); problems.append("no .bnr in tools/build/")

    print("--- Morrowind content ---")
    data = os.path.join(mw, "Data Files")
    for f in ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm",
              "Morrowind.bsa", "Tribunal.bsa", "Bloodmoon.bsa"):
        p = os.path.join(data, f)
        if os.path.isfile(p):
            print("  OK      %-22s %8.1f MiB" % (f, os.path.getsize(p) / 1048576.0))
        else:
            print("  MISSING " + f)
            problems.append("missing " + f + " in " + data)
    snd = os.path.join(data, "Sound")
    if os.path.isdir(snd):
        n = sum(len(fs) for _, _, fs in os.walk(snd))
        print("  OK      Sound/ (%d files)" % n)
    else:
        print("  MISSING Sound/")
        problems.append("no Sound folder in Data Files")

    print("--- converter patch ---")
    markers = count_markers(root)
    if markers == len(MARKERS):
        print("  OK      converter patched (%d/%d)" % (markers, len(MARKERS)))
    elif markers == 0:
        print("  PENDING not patched yet (will patch on first convert)")
        fixable.append("converter")
    else:
        print("  BROKEN  partly patched (%d/%d)" % (markers, len(MARKERS)))
        problems.append("converter partly patched - run --task restore, then rerun")

    print()
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  - " + p)
    else:
        print("All dependencies OK.")
    return problems, fixable


# ==========================================================================
# converter patch
# ==========================================================================
MWFILES_HELPERS = '''\
_LOOSE_INDEX = None


def loose_path(name):
    global _LOOSE_INDEX
    if _LOOSE_INDEX is None:
        _LOOSE_INDEX = {}
        base = str(DATA_FILES)
        sound = os.path.join(base, "Sound")
        if os.path.isdir(sound):
            for root, _dirs, files in os.walk(sound):
                for f in files:
                    rel = os.path.relpath(os.path.join(root, f), base)
                    _LOOSE_INDEX[rel.replace(os.sep, "/").lower()] = os.path.join(root, f)
    return _LOOSE_INDEX.get(name.replace(chr(92), "/").lower().lstrip("/"))


def read_masters(wanted=None):
    masters = [DATA_FILES / n for n in ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm")
               if (DATA_FILES / n).exists()]
    for m in masters:
        print(f"  reading {m.name} ...")
        yield from read_records(m, wanted)


def _clone_record(base, subs):
    try:
        return type(base)(base.tag, base.flags, subs)
    except (TypeError, AttributeError):
        import copy
        new = copy.copy(base)
        new.subs = subs
        return new


def merge_cell(base, over):
    import struct
    if base is None:
        return over

    def split(rec):
        head, refs, cur = [], {}, None
        for tag, d in rec.subs:
            if tag == "FRMR":
                cur = [(tag, d)]
                refs[struct.unpack("<I", d)[0]] = cur
            elif cur is None:
                head.append((tag, d))
            else:
                cur.append((tag, d))
        return head, refs

    h0, r0 = split(base)
    h1, r1 = split(over)
    r0.update(r1)
    subs = h1 + [s for blk in r0.values() for s in blk]
    return _clone_record(base, subs)


'''

EDITS = {
    "tools/convert/mwfiles.py": [
        ('def load_db(path=None):\n'
         '    """One pass over Morrowind.esm collecting everything the level tools use."""\n',
         MWFILES_HELPERS +
         'def load_db(path=None):\n'
         '    """One pass over the installed masters collecting everything the level tools use."""\n'),
        ('    for r in read_records(path or DATA_FILES / "Morrowind.esm"):\n',
         '    for r in (read_records(path) if path else read_masters()):\n'),
        ('    for r in (read_records(path) if path else read_masters()):\n'
         '        t = r.tag\n',
         '    for r in (read_records(path) if path else read_masters()):\n'
         '        if r.flags & 0x20:\n'
         '            continue\n'
         '        t = r.tag\n'),
        ('        elif t == "CELL":\n'
         '            if struct.unpack("<I", r.get("DATA")[:4])[0] & 1:\n'
         '                db["CELL"][r.id] = r\n'
         '            else:\n'
         '                db["CELL"][struct.unpack("<ii", r.get("DATA")[4:12])] = r\n',
         '        elif t == "CELL":\n'
         '            if struct.unpack("<I", r.get("DATA")[:4])[0] & 1:\n'
         '                key = r.id\n'
         '            else:\n'
         '                key = struct.unpack("<ii", r.get("DATA")[4:12])\n'
         '            db["CELL"][key] = merge_cell(db["CELL"].get(key), r)\n'),
        ('        if t == "DIAL":\n'
         '            dial = {"name": r.id, "type": r.get("DATA")[0], "infos": []}\n'
         '            db["dialogue"].append(dial)\n',
         '        if t == "DIAL":\n'
         '            dial = next((d for d in db["dialogue"] if d["name"].lower() == r.id.lower()), None)\n'
         '            if dial is None:\n'
         '                dial = {"name": r.id, "type": r.get("DATA")[0], "infos": []}\n'
         '                db["dialogue"].append(dial)\n'),
        ('    def exists(self, name):\n'
         '        name = name.lower().replace("/", "\\\\")\n'
         '        return any((d / name).exists() for d in OVERLAYS) or (DATA_FILES / name).exists() or any(name in b.files for b in self.bsas)\n',
         '    def exists(self, name):\n'
         '        name = name.lower().replace("/", "\\\\")\n'
         '        alt = name.replace("\\\\", "/")\n'
         '        return any((d / name).exists() or (d / alt).exists() for d in OVERLAYS) or loose_path(name) is not None or any(name in b.files for b in self.bsas)\n'),
        ('        for d in reversed(OVERLAYS):\n'
         '            if (d / name).exists():\n'
         '                return (d / name).read_bytes()\n'
         '        loose = DATA_FILES / name\n'
         '        if loose.exists():\n'
         '            return loose.read_bytes()\n',
         '        for d in reversed(OVERLAYS):\n'
         '            if (d / name).exists():\n'
         '                return (d / name).read_bytes()\n'
         '            if (d / name.replace("\\\\", "/")).exists():\n'
         '                return (d / name.replace("\\\\", "/")).read_bytes()\n'
         '        loose = loose_path(name)\n'
         '        if loose is not None:\n'
         '            return loose.read_bytes()\n'),
    ],
    "tools/convert/build_game.py": [
        ("from mwfiles import DATA_FILES, read_records\n",
         "from mwfiles import DATA_FILES, read_masters, read_records\n"),
        ('for r in read_records(DATA_FILES / "Morrowind.esm", {"MGEF"}):\n',
         'for r in read_masters({"MGEF"}):\n', 2),
        ('for r in read_records(DATA_FILES / "Morrowind.esm", {"ENCH"}):\n',
         'for r in read_masters({"ENCH"}):\n', 1),
    ],
    "tools/convert/level.py": [
        ('from mwfiles import DATA_FILES, Archives, cell_refs, load_db, read_records\n',
         'from mwfiles import DATA_FILES, Archives, cell_refs, load_db, read_masters, read_records\n'),
        ('for r in read_records(DATA_FILES / "Morrowind.esm", {"REGN"}):\n',
         'for r in read_masters({"REGN"}):\n'),
        ('for r in read_records(DATA_FILES / "Morrowind.esm", {"LEVC"}):\n',
         'for r in read_masters({"LEVC"}):\n'),
    ],
}

MARKERS = [
    ("read_masters helper",  "tools/convert/mwfiles.py", "def read_masters("),
    ("load_db uses masters", "tools/convert/mwfiles.py", "read_masters()):"),
    ("deleted records skip", "tools/convert/mwfiles.py", "r.flags & 0x20"),
    ("cell ref merge",       "tools/convert/mwfiles.py", "def merge_cell("),
    ("cells use merge_cell", "tools/convert/mwfiles.py", 'merge_cell(db["CELL"].get(key), r)'),
    ("dialogue merge",       "tools/convert/mwfiles.py", 'd["name"].lower() == r.id.lower()'),
    ("loose index helper",   "tools/convert/mwfiles.py", "def loose_path("),
    ("exists uses index",    "tools/convert/mwfiles.py", "loose_path(name) is not None"),
    ("read uses index",      "tools/convert/mwfiles.py", "loose = loose_path(name)"),
    ("linux loose paths b",  "tools/convert/mwfiles.py", '(d / name.replace("\\\\", "/")).exists()'),
    ("level.py import",      "tools/convert/level.py",   "read_masters,"),
    ("level.py REGN",        "tools/convert/level.py",   'read_masters({"REGN"})'),
    ("level.py LEVC",        "tools/convert/level.py",   'read_masters({"LEVC"})'),
    ("build_game import",    "tools/convert/build_game.py", "read_masters,"),
    ("build_game MGEF",      "tools/convert/build_game.py", 'read_masters({"MGEF"})', 2),
    ("build_game ENCH",      "tools/convert/build_game.py", 'read_masters({"ENCH"})'),
]


def count_markers(root):
    ok = 0
    for m in MARKERS:
        path, needle = m[1], m[2]
        need = m[3] if len(m) > 3 else 1
        try:
            with open(os.path.join(root, path), encoding="utf-8") as f:
                if f.read().count(needle) >= need:
                    ok += 1
        except OSError:
            pass
    return ok


def patch_converter(root, quiet=False):
    if count_markers(root) == len(MARKERS):
        if not quiet:
            print("  converter already patched")
        return
    for path, edits in EDITS.items():
        full = os.path.join(root, path)
        try:
            s = open(full, encoding="utf-8").read()
        except OSError as e:
            sys.exit("cannot read %s: %s" % (path, e))
        for i, edit in enumerate(edits):
            old, rep = edit[0], edit[1]
            need = edit[2] if len(edit) > 2 else 1
            n = s.count(old)
            if n != need:
                sys.exit("%s edit %d matched %d times (need %d) - NOTHING was changed"
                         % (path, i + 1, n, need))
            s = s.replace(old, rep)
        shutil.copyfile(full, full + BAK)
        open(full, "w", encoding="utf-8").write(s)
        if not quiet:
            print("  patched " + path)
    if count_markers(root) != len(MARKERS):
        sys.exit("converter patch verification failed")


def restore_converter(root):
    found = False
    for path in EDITS:
        full = os.path.join(root, path)
        if os.path.exists(full + BAK):
            shutil.copyfile(full + BAK, full)
            os.remove(full + BAK)
            print("  restored " + path)
            found = True
    if not found:
        print("  nothing to restore")


# ==========================================================================
# fix helpers
# ==========================================================================
def setup_venv(root):
    venv = _venv_python(root)
    needs = required_python_modules(root)
    if os.path.isfile(venv) and _has_modules(venv, needs):
        print("  .venv already ready")
        return venv
    if not os.path.isfile(venv):
        print("  creating .venv ...")
        r = subprocess.run([sys.executable, "-m", "venv", os.path.join(root, ".venv")])
        if r.returncode != 0:
            hint = ("sudo apt install python3-venv python3-pip" if IS_LINUX
                    else "install Python's venv module")
            sys.exit("  could not create venv - " + hint)
    print("  pip install -r requirements.txt ...")
    subprocess.run([venv, "-m", "pip", "install", "-r",
                    os.path.join(root, "requirements.txt")])
    if not _has_modules(venv, needs):
        print("  WARNING: some modules still fail to import")
    return venv


def offer_fixes(root, problems, fixable):
    """Return True if any fix was applied, False otherwise."""
    if not fixable:
        return False
    print()
    print("The following things can be fixed automatically:")
    if "python" in fixable:    print("  - set up .venv and pip install requirements")
    if "converter" in fixable: print("  - patch the converter for the expansions")
    try:
        ans = input("Fix now? [Y/n] ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        return False
    if ans not in ("", "y", "yes"):
        return False
    if "python" in fixable:
        setup_venv(root)
    if "converter" in fixable and count_markers(root) == 0:
        patch_converter(root)
    return True


# ==========================================================================
# tasks
# ==========================================================================
def do_convert(root, mw):
    py, _missing = python_ok(root)
    if not py:
        py = setup_venv(root)
    patch_converter(root)
    env = dict(os.environ, MW3DS_MORROWIND_DIR=mw)
    log = os.path.join(root, "convert.log")
    print()
    print("=== converting game data (30-60+ min) ===")
    print("  log: " + log)
    cmd = [py, "tools/convert/level.py", "--world"]
    if not IS_WINDOWS:
        nice = shutil.which("nice")
        if nice:
            cmd = [nice, "-n", "10"] + cmd
    with open(log, "w", encoding="utf-8") as lf:
        p = subprocess.Popen(cmd, cwd=root, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True,
                             errors="replace", bufsize=1)
        buf = []
        for line in p.stdout:
            buf.append(line)
            if len(buf) >= 200:
                chunk = "".join(buf); sys.stdout.write(chunk); lf.write(chunk); buf.clear()
        if buf:
            chunk = "".join(buf); sys.stdout.write(chunk); lf.write(chunk)
        p.wait()
    if p.returncode != 0:
        sys.exit("  conversion failed - see " + log)
    print("  conversion done")


def do_build_cia(root):
    if not os.path.isdir(os.path.join(root, "out", "data")):
        sys.exit("  out/data missing - run the convert step first")

    makerom = find_makerom(root)
    if not makerom:
        sys.exit("  makerom missing for this platform - expected " + _makerom_name())
    _chmod_x(makerom)

    rsf = glob.glob(os.path.join(root, "tools", "build", "*.rsf"))
    bnr = glob.glob(os.path.join(root, "tools", "build", "*.bnr"))
    if not rsf or not bnr:
        sys.exit("  tools/build/ needs an .rsf and a .bnr banner")

    env = _devkit_env()
    if not shutil.which("make", path=env["PATH"]):
        sys.exit("  make not found on PATH (devkitPro on Windows, "
                 "Xcode CLT on macOS, build-essential on Linux)")

    jobs = min(os.cpu_count() or 1, MAX_JOBS)
    print()
    print("=== building engine ===")
    _run(["make", "clean"], root, env)
    _run(["make", "-j%d" % jobs], root, env)

    for f in ("mw3ds.elf", "mw3ds.smdh"):
        if not os.path.isfile(os.path.join(root, f)):
            sys.exit("  %s was not produced - check the make output above" % f)

    print()
    print("=== staging romfs ===")
    romfs = os.path.join(root, "romfs")
    _rmtree(romfs)
    os.makedirs(romfs, exist_ok=True)
    _copy_tree(os.path.join(root, "out", "data"), os.path.join(romfs, "data"))

    print()
    print("=== running makerom ===")
    cia = os.path.join(root, "build", "mw3ds.cia")
    if os.path.exists(cia):
        os.remove(cia)
    cmd = [
        makerom, "-f", "cia", "-o", "build/mw3ds.cia",
        "-elf", "mw3ds.elf",
        "-rsf", os.path.relpath(rsf[0], root),
        "-icon", "mw3ds.smdh",
        "-banner", os.path.relpath(bnr[0], root),
        "-exefslogo", "-target", "t", "-ignoresign",
        "-DAPP_TITLE=MW3DS",
        "-DAPP_PRODUCT_CODE=CTR-P-MW3D",
        "-DAPP_UNIQUE_ID=0xF3D50",
        "-DAPP_ROMFS=romfs",
        "-DAPP_CATEGORY=Application",
        "-DAPP_USE_ON_SD=true",
        "-DAPP_ENCRYPTED=false",
        "-DAPP_MEMORY_TYPE=Application",
        "-DAPP_SYSTEM_MODE=64MB",
        "-DAPP_SYSTEM_MODE_EXT=124MB",
        "-DAPP_CPU_SPEED=804MHz",
        "-DAPP_ENABLE_L2_CACHE=true",
        "-DAPP_VERSION_MAJOR=0",
    ]
    _run(cmd, root, env)

    if not os.path.isfile(cia):
        sys.exit("  build/mw3ds.cia was not produced")
    print()
    print("  CIA: %s (%.1f MiB)" % (cia, os.path.getsize(cia) / 1048576.0))


def do_verify(root):
    print()
    print("=== pre-install verification ===")
    bad = 0
    if count_markers(root) != len(MARKERS):
        print("  FAIL converter not patched"); bad += 1
    else:
        print("  OK   converter patched")
    outdata = os.path.join(root, "out", "data")
    n_cells = sum(1 for _, _, fs in os.walk(os.path.join(outdata, "cells")) for f in fs)
    n_snd   = sum(1 for _, _, fs in os.walk(os.path.join(outdata, "snd"))
                   for f in fs if f.endswith(".snd"))
    print("  cells: %d   sound: %d" % (n_cells, n_snd))
    if n_cells == 0: print("  FAIL no cells"); bad += 1
    if n_snd < 2000: print("  WARN few sound files"); bad += 1
    for f in ("mw3ds.elf", "mw3ds.smdh"):
        if os.path.exists(os.path.join(root, f)): print("  OK   " + f)
        else: print("  FAIL " + f + " missing"); bad += 1
    cia = os.path.join(root, "build", "mw3ds.cia")
    if os.path.exists(cia):
        print("  OK   cia %.1f MiB" % (os.path.getsize(cia) / 1048576.0))
    else:
        print("  FAIL cia missing"); bad += 1
    print()
    print("ALL OK" if not bad else "%d problem(s)" % bad)
    return 0 if not bad else 1


# ==========================================================================
# menu / main
# ==========================================================================
def menu():
    print()
    print("=" * 52)
    print(" MW3DS - choose a task")
    print("=" * 52)
    print("  1) Convert game data only")
    print("  2) Build the CIA only (needs out/data)")
    print("  3) Convert data + build the CIA")
    print("  4) Re-run the dependency check")
    print("  5) Verify the result")
    print("  6) Restore converter originals")
    print("  0) Quit")
    try:
        return input("> ").strip()
    except (EOFError, KeyboardInterrupt):
        return "0"


def main():
    ap = argparse.ArgumentParser(description="MW3DS launcher")
    ap.add_argument("--task", choices=("convert", "cia", "all", "check",
                                        "verify", "restore"), default=None)
    ap.add_argument("--morrowind-dir", default=None)
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--reset", action="store_true",
                    help="forget the saved Morrowind path")
    args = ap.parse_args()

    gc.disable()
    root = HERE
    os.chdir(root)

    if args.reset:
        if os.path.isfile(CFG):
            os.remove(CFG)
            print("  removed mw3ds.cfg")
        return 0

    cfg = load_cfg()
    mw = ask_morrowind(cfg, args.morrowind_dir)

    problems, fixable = check_all(root, mw)
    if args.check_only:
        return 0 if not problems else 1

    # Only recheck if there was something fixable AND the user agreed to fix it.
    if fixable:
        applied = offer_fixes(root, problems, fixable)
        if applied:
            print()
            print("=== re-checking after fixes ===")
            problems, fixable = check_all(root, mw)

    if problems:
        print()
        print("Cannot continue until the problems above are fixed.")
        return 1

    task = args.task
    if task is None:
        task = {"1": "convert", "2": "cia", "3": "all",
                "4": "check", "5": "verify", "6": "restore",
                "0": None}.get(menu())
        if not task:
            return 0
        if task in ("convert", "all"):
            saved = cfg.get("mw3ds", "morrowind_dir", fallback="")
            try:
                change = input("Morrowind path [%s] (press Enter to keep, or type new): " % saved).strip()
            except (EOFError, KeyboardInterrupt):
                change = ""
            if change:
                if valid_morrowind(change):
                    mw = change
                    cfg["mw3ds"]["morrowind_dir"] = mw
                    save_cfg(cfg)
                    print()
                    print("=== re-checking with new Morrowind path ===")
                    problems, fixable = check_all(root, mw)
                    if problems:
                        print()
                        print("Cannot continue until the problems above are fixed.")
                        return 1
                else:
                    print("  not a valid Morrowind root (no 'Data Files'), keeping the old one")

    if task == "restore":
        restore_converter(root); return 0
    if task == "verify":
        return do_verify(root)
    if task == "check":
        return 0 if not problems else 1
    if task == "convert":
        do_convert(root, mw)
        return 0
    if task == "cia":
        do_build_cia(root)
        return 0
    if task == "all":
        do_convert(root, mw)
        do_build_cia(root)
        return do_verify(root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
