#!/usr/bin/env python3
"""mw3ds_full.py - the whole job in one run:

  1. scan your tree (prerequisites, expansions' ESMs/BSAs, converter patch state)
  2. patch the converter so it reads Tribunal.esm and Bloodmoon.esm too
  3. run the data conversion (tools/convert/level.py --world) - 30-60+ min
  4. compile the engine (make)
  5. package the CIA (makerom)
  6. verify the result before you install

Defaults are set for YOUR machine:
    project folder:  /home/joe/Documents/Morrowind3DS_project/MW3DS
    Morrowind root:  /home/joe/games/Morrowind
    makerom:         ./makerom   (the copy in the project folder)

From inside the project folder:
    python3 mw3ds_full.py

Modes:
    --verify-only     just run the pre-install checks
    --no-convert      rebuild engine + CIA from existing out/data
    --restore         undo the converter patch (uses the .bak files)
    --check-sound     test sound-file lookup on this disk and exit

Performance: make is capped at 4 jobs (override with --make), subprocesses run
under nice -n 10, loose_path() indexes only Data Files/Sound/ lazily, the
romfs staging copy streams with progress, and converter output is buffered.
"""
import argparse
import gc
import glob
import os
import re
import shutil
import subprocess
import sys

# ---- your paths ---------------------------------------------------------
DEFAULT_PROJECT = "/home/joe/Documents/Morrowind3DS_project/MW3DS"
DEFAULT_MORROWIND = "/home/joe/games/Morrowind"

BAK = ".pre-expansions.bak"
NICE = ["nice", "-n", "10"]
MAX_JOBS = 4

# =====================================================================
# PART 1: converter edits
# =====================================================================

MWFILES_HELPERS = '''\
_LOOSE_INDEX = None


def loose_path(name):
    """Case-insensitive, slash-agnostic lookup of a loose file under Data Files.
    Only Sound/ is indexed (that is the only subtree the converter looks up
    through this path); the index is built once, lazily, and holds plain
    strings rather than Path objects. Returns the real path as a string, or
    None."""
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
    """Records from every installed master, in load order: Morrowind.esm,
    then Tribunal.esm, then Bloodmoon.esm (later ones override earlier ones)."""
    masters = [DATA_FILES / n for n in ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm")
               if (DATA_FILES / n).exists()]
    for m in masters:
        print(f"  reading {m.name} ...")
        yield from read_records(m, wanted)


def _clone_record(base, subs):
    """A copy of base carrying a new subrecord list, whatever Record is."""
    try:
        return type(base)(base.tag, base.flags, subs)
    except (TypeError, AttributeError):
        import copy
        new = copy.copy(base)
        new.subs = subs
        return new


def merge_cell(base, over):
    """A later master's CELL record carries only the refs it adds or changes
    (matched by FRMR value); the base cell's other refs stay."""
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
        (
            'def load_db(path=None):\n'
            '    """One pass over Morrowind.esm collecting everything the level tools use."""\n',
            MWFILES_HELPERS +
            'def load_db(path=None):\n'
            '    """One pass over the installed masters collecting everything the level tools use."""\n',
        ),
        (
            '    for r in read_records(path or DATA_FILES / "Morrowind.esm"):\n',
            '    for r in (read_records(path) if path else read_masters()):\n',
        ),
        (
            '    for r in (read_records(path) if path else read_masters()):\n'
            '        t = r.tag\n',
            '    for r in (read_records(path) if path else read_masters()):\n'
            '        if r.flags & 0x20:      # deleted: skip it\n'
            '            continue\n'
            '        t = r.tag\n',
        ),
        (
            '        elif t == "CELL":\n'
            '            if struct.unpack("<I", r.get("DATA")[:4])[0] & 1:\n'
            '                db["CELL"][r.id] = r\n'
            '            else:\n'
            '                db["CELL"][struct.unpack("<ii", r.get("DATA")[4:12])] = r\n',
            '        elif t == "CELL":\n'
            '            if struct.unpack("<I", r.get("DATA")[:4])[0] & 1:\n'
            '                key = r.id\n'
            '            else:\n'
            '                key = struct.unpack("<ii", r.get("DATA")[4:12])\n'
            '            db["CELL"][key] = merge_cell(db["CELL"].get(key), r)\n',
        ),
        (
            '        if t == "DIAL":\n'
            '            dial = {"name": r.id, "type": r.get("DATA")[0], "infos": []}\n'
            '            db["dialogue"].append(dial)\n',
            '        if t == "DIAL":\n'
            '            dial = next((d for d in db["dialogue"] if d["name"].lower() == r.id.lower()), None)\n'
            '            if dial is None:\n'
            '                dial = {"name": r.id, "type": r.get("DATA")[0], "infos": []}\n'
            '                db["dialogue"].append(dial)\n',
        ),
        (
            '    def exists(self, name):\n'
            '        name = name.lower().replace("/", "\\\\")\n'
            '        return any((d / name).exists() for d in OVERLAYS) or (DATA_FILES / name).exists() or any(name in b.files for b in self.bsas)\n',
            '    def exists(self, name):\n'
            '        name = name.lower().replace("/", "\\\\")\n'
            '        alt = name.replace("\\\\", "/")\n'
            '        return any((d / name).exists() or (d / alt).exists() for d in OVERLAYS) or loose_path(name) is not None or any(name in b.files for b in self.bsas)\n',
        ),
        (
            '        for d in reversed(OVERLAYS):\n'
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
            '            return loose.read_bytes()\n',
        ),
    ],

    "tools/convert/build_game.py": [
        (
            "from mwfiles import DATA_FILES, read_records\n",
            "from mwfiles import DATA_FILES, read_masters, read_records\n",
        ),
        (
            'for r in read_records(DATA_FILES / "Morrowind.esm", {"MGEF"}):\n',
            'for r in read_masters({"MGEF"}):\n',
            2,
        ),
        (
            'for r in read_records(DATA_FILES / "Morrowind.esm", {"ENCH"}):\n',
            'for r in read_masters({"ENCH"}):\n',
            1,
        ),
    ],

    "tools/convert/level.py": [
        (
            'from mwfiles import DATA_FILES, Archives, cell_refs, load_db, read_records\n',
            'from mwfiles import DATA_FILES, Archives, cell_refs, load_db, read_masters, read_records\n',
        ),
        (
            'for r in read_records(DATA_FILES / "Morrowind.esm", {"REGN"}):\n',
            'for r in read_masters({"REGN"}):\n',
        ),
        (
            'for r in read_records(DATA_FILES / "Morrowind.esm", {"LEVC"}):\n',
            'for r in read_masters({"LEVC"}):\n',
        ),
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


def check_markers(root, verbose=True):
    ok = 0
    for m in MARKERS:
        what, path, needle = m[0], m[1], m[2]
        need = m[3] if len(m) > 3 else 1
        try:
            with open(os.path.join(root, path), encoding="utf-8") as f:
                n = f.read().count(needle)
        except OSError:
            n = 0
        good = n >= need
        if verbose:
            extra = " x%d" % n if need > 1 else ""
            print("  %-24s %s%s" % (what, "present" if good else "MISSING", extra))
        ok += good
    return ok


def patch_converter(root):
    present = check_markers(root, verbose=False)
    if present == len(MARKERS):
        print("  converter already patched - leaving it alone")
        return
    if present:
        sys.exit("  The converter is PARTLY patched (%d of %d markers). Restore the\n"
                 "  originals and rerun:\n"
                 "    python3 mw3ds_full.py --restore" % (present, len(MARKERS)))

    for path, edits in EDITS.items():
        full = os.path.join(root, path)
        try:
            with open(full, encoding="utf-8") as f:
                s = f.read()
        except OSError as e:
            sys.exit("cannot read %s: %s" % (path, e))
        for i, edit in enumerate(edits):
            old, rep = edit[0], edit[1]
            need = edit[2] if len(edit) > 2 else 1
            n = s.count(old)
            if n != need:
                sys.exit("%s: edit %d matched %d times (need exactly %d); NOTHING was changed.\n"
                         "Your converter source differs from what this script expects.\n"
                         "Expected text:\n%s" % (path, i + 1, n, need, old[:400]))
            s = s.replace(old, rep)
        shutil.copyfile(full, full + BAK)
        with open(full, "w", encoding="utf-8") as f:
            f.write(s)
        print("  patched " + path)
        del s

    if check_markers(root) != len(MARKERS):
        sys.exit("verification failed - run: python3 mw3ds_full.py --restore")
    print("  converter verified: all three masters, cells merged per-FRMR, Linux sound fix in")


def restore_originals(root):
    found = False
    for path in EDITS:
        full = os.path.join(root, path)
        if os.path.exists(full + BAK):
            shutil.copyfile(full + BAK, full)
            os.remove(full + BAK)
            print("  restored " + path)
            found = True
    if not found:
        print("  no %s files found - nothing to restore" % BAK)


# =====================================================================
# PART 2: helpers
# =====================================================================

def find_build_assets(root):
    bdir = os.path.join(root, "tools", "build")

    def pick(pattern, preferred):
        hits = sorted(glob.glob(os.path.join(bdir, pattern)))
        for h in hits:
            if os.path.basename(h) == preferred:
                return h
        return hits[0] if hits else None

    return pick("*.rsf", "mw3ds.rsf"), pick("*.bnr", "banner.bnr")


def scan_tree(root, morrowind_dir):
    print("=== step 0: scanning your tree ===")
    problems = []
    for path in EDITS:
        if not os.path.exists(os.path.join(root, path)):
            problems.append("missing " + path)
    n = check_markers(root, verbose=False)
    print("  converter patch: %s" % ("applied" if n == len(MARKERS)
                                     else "partly applied" if n else "not applied yet"))
    data = os.path.join(morrowind_dir, "Data Files")
    for f in ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm",
              "Morrowind.bsa", "Tribunal.bsa", "Bloodmoon.bsa"):
        p = os.path.join(data, f)
        if os.path.exists(p):
            print("  %-40s %10.1f MiB" % (f, os.path.getsize(p) / 1048576.0))
        else:
            problems.append("missing " + f + " in " + data)
    snd = os.path.join(data, "Sound")
    if os.path.isdir(snd):
        count = sum(len(fs) for _, _, fs in os.walk(snd))
        print("  %-40s %10d files" % ("Sound/", count))
        if count < 100:
            problems.append("Sound folder nearly empty - where are the .wav/.mp3 files?")
    else:
        problems.append("no Sound folder in Data Files - the game's sound effects")
    for f in ("Makefile", "requirements.txt"):
        if not os.path.exists(os.path.join(root, f)):
            problems.append("missing " + f + " in the project folder")
    rsf, bnr = find_build_assets(root)
    if not rsf:
        problems.append("no .rsf file in tools/build/")
    if not bnr:
        problems.append("no prebuilt .bnr banner in tools/build/")
    if not (os.path.isfile(os.path.join(root, "makerom")) or shutil.which("makerom")):
        problems.append("makerom not found (expected in the project folder)")
    if problems:
        print("  PROBLEMS:")
        for p in problems:
            print("    - " + p)
        return False
    print("  scan ok")
    return True


def _walk_count(path, suffix):
    n = 0
    for dirpath, dirs, files in os.walk(path):
        n += sum(1 for f in files if f.endswith(suffix))
    return n


def verify_result(root, expansions=True):
    print("")
    print("=== pre-install verification ===")
    bad = 0
    warn = 0

    if expansions:
        print("--- converter markers ---")
        ok = check_markers(root)
        bad += len(MARKERS) - ok

    outdata = os.path.join(root, "out", "data")
    cells = os.path.join(outdata, "cells")
    names = []
    if os.path.isdir(cells):
        for dirpath, dirs, files in os.walk(cells):
            names.extend(files)

    def count(pred):
        return sum(1 for n in names if pred(n.lower()))

    if expansions:
        print("--- expansion content (%d cell files) ---" % len(names))
        if names:
            tribunal = count(lambda n: any(k in n for k in ("mournhold", "godsr", "grengrim", "plaza brin", "bazaar")))
            bloodmoon = count(lambda n: any(k in n for k in ("solstheim", "skaal", "thirsk", "fort frostmoth", "raven rock")))
            print("  Tribunal cells (Mournhold...):  %d" % tribunal)
            print("  Bloodmoon cells (Solstheim...): %d" % bloodmoon)
            if tribunal == 0:
                print("  WARNING: no Mournhold cells found - did the conversion read Tribunal.esm?")
                warn += 1
            if bloodmoon == 0:
                print("  WARNING: no Solstheim cells found - did the conversion read Bloodmoon.esm?")
                warn += 1
        else:
            print("  (no out/data/cells folder - cell checks skipped)")

    print("--- sound ---")
    n_snd = _walk_count(os.path.join(outdata, "snd"), ".snd")
    n_mus = _walk_count(os.path.join(outdata, "music"), ".snd")
    print("  sound/voice files (snd/, recursive): %d" % n_snd)
    print("  music files: %d" % n_mus)
    if n_snd < 2000:
        print("  WARNING: too few sound files - the sound conversion looks incomplete")
        warn += 1

    print("--- build outputs ---")
    for f in ("mw3ds.elf", "mw3ds.smdh"):
        p = os.path.join(root, f)
        if os.path.exists(p):
            print("  %-12s %8.1f MiB" % (f, os.path.getsize(p) / 1048576.0))
        else:
            print("  MISSING: " + f)
            bad += 1

    print("--- the CIA ---")
    cia = os.path.join(root, "build", "mw3ds.cia")
    if os.path.exists(cia):
        mib = os.path.getsize(cia) / 1048576.0
        print("  build/mw3ds.cia: %.1f MiB" % mib)
        if mib < 900:
            print("  WARNING: smaller than expected (~1.3+ GiB with both expansions)")
            warn += 1
    else:
        print("  MISSING: build/mw3ds.cia")
        bad += 1

    if os.path.isdir(outdata):
        total = 0
        for dirpath, dirs, files in os.walk(outdata):
            for f in files:
                total += os.path.getsize(os.path.join(dirpath, f))
        print("  out/data: %.1f GiB" % (total / 1073741824.0))

    print("")
    if bad:
        print("  %d problem(s) found - see above. Do not install yet." % bad)
    elif warn:
        print("  no hard errors, %d warning(s) - read them above before installing" % warn)
    else:
        print("  ALL CHECKS PASSED - safe to install build/mw3ds.cia")
    return bad == 0


_IMPORT_NAMES = {"pillow": "PIL", "audioop-lts": "audioop"}


def needed_imports(root):
    names = []
    try:
        with open(os.path.join(root, "requirements.txt"), encoding="utf-8") as f:
            for line in f:
                line = line.split("#")[0].strip()
                m = re.match(r"[A-Za-z0-9_.\-]+", line)
                if m:
                    pkg = m.group(0).lower()
                    names.append(_IMPORT_NAMES.get(pkg, pkg.replace("-", "_")))
    except OSError:
        pass
    return names or ["audioop", "numpy", "PIL", "miniaudio"]


def find_python(root):
    needs = needed_imports(root)
    probe = "import " + ", ".join(needs)

    def has_modules(py):
        return subprocess.run([py, "-c", probe], capture_output=True).returncode == 0

    venv_dir = os.path.join(root, ".venv")
    venv_py = os.path.join(venv_dir, "bin", "python")
    cands = ([venv_py] if os.path.exists(venv_py) else []) + [sys.executable]
    for py in cands:
        if has_modules(py):
            if py != sys.executable:
                print("  using " + py)
            return py

    print("  converter modules missing (%s): setting up %s ..." % (", ".join(needs), venv_dir))
    if not os.path.exists(venv_py):
        r = subprocess.run([sys.executable, "-m", "venv", venv_dir])
        if r.returncode != 0:
            sys.exit("ERROR: could not create a virtualenv. On Debian/antiX run:\n"
                     "  sudo apt install python3-venv python3-pip")
    subprocess.run(NICE + [venv_py, "-m", "pip", "install", "-r", os.path.join(root, "requirements.txt")])
    if not has_modules(venv_py):
        print("  WARNING: some converter modules still fail to import")
    else:
        print("  using " + venv_py)
    return venv_py


def find_makerom(root, arg):
    cands = []
    if arg:
        cands.append(arg)
    cands.append(os.path.join(root, "makerom"))
    which = shutil.which("makerom")
    if which:
        cands.append(which)
    for c in cands:
        if c and os.path.isfile(c):
            if not os.access(c, os.X_OK):
                print("  chmod +x " + c)
                os.chmod(c, 0o755)
            return c
    sys.exit("ERROR: makerom not found. Put it in the project folder or pass --makerom")


def stage_pclink(root, pc_address):
    src = os.path.join(root, "source")
    inc = os.path.join(root, "include")
    have = [f for f in ("pclink.c", "pclink.cpp", "pclink.h", "pclink_frame.h", "pclink_frame.cpp")
            if os.path.exists(os.path.join(src, f)) or os.path.exists(os.path.join(inc, f))]
    main_cpp = os.path.join(src, "main.cpp")
    text = open(main_cpp, errors="replace").read() if os.path.exists(main_cpp) else ""
    if "pclink_update" not in text:
        print("  PC Link: source/main.cpp has no pclink hooks - skipped")
        return False
    if ("pclink.c" in have or "pclink.cpp" in have) and "pclink.h" in have:
        print("  PC Link: using the pclink files already in source/ (%s)" % ", ".join(have))
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        for d in (os.path.join(here, "mw3ds-pclink"), here, os.path.join(root, "mw3ds-pclink")):
            if os.path.exists(os.path.join(d, "pclink.c")) and os.path.exists(os.path.join(d, "pclink.h")):
                break
        else:
            sys.exit("ERROR: main.cpp uses PC Link but source/pclink.c + pclink.h are missing")
        shutil.copy2(os.path.join(d, "pclink.c"), os.path.join(src, "pclink.c"))
        shutil.copy2(os.path.join(d, "pclink.h"), os.path.join(inc if os.path.isdir(inc) else src, "pclink.h"))
        print("  PC Link: copied pclink files from " + d)
    if "pclink_frame.h" in text and "pclink_frame.h" not in have:
        sys.exit("ERROR: main.cpp includes pclink_frame.h but it does not exist")
    if pc_address:
        print("  PC Link: baking default PC address %s into the romfs" % pc_address)
    return True


def run(cmd, what, cwd=None, env=None, polite=True):
    full = (NICE + list(cmd)) if polite else list(cmd)
    print("  $ " + " ".join(full))
    r = subprocess.run(full, cwd=cwd, env=env)
    if r.returncode != 0:
        sys.exit("ERROR: %s failed (exit code %d)" % (what, r.returncode))
    print("  " + what + " ok")


def check_sound(morrowind_dir):
    from pathlib import Path
    data = Path(morrowind_dir) / "Data Files"
    print("=== sound lookup check ===")
    print("  top-level folders in Data Files: " +
          ", ".join(sorted(p.name for p in data.iterdir() if p.is_dir())))
    files = [p for p in data.rglob("*") if p.is_file()
             and p.relative_to(data).parts[0].lower() == "sound"]
    print("  files under Sound (any case): %d" % len(files))
    if not files:
        print("  No loose Sound files at all - effects must come from the .bsa files.")
        return
    rels = [p.relative_to(data) for p in files]
    old = sum(1 for r in rels if (data / r.as_posix().lower()).exists())
    index = {r.as_posix().lower(): 1 for r in rels}
    new = sum(1 for r in rels if r.as_posix().lower() in index)
    print("  found by the OLD lowercase path lookup: %d of %d" % (old, len(rels)))
    print("  found by the case-insensitive index:    %d of %d" % (new, len(rels)))
    if old < len(rels):
        print("  -> the old lookup MISSES files: this is why sound was missing.")


def copy_tree_streaming(src, dst):
    """Copy src to dst one file at a time with progress every 500 files.
    Never accumulates a full directory list, unlike shutil.copytree."""
    total = sum(len(f) for _, _, f in os.walk(src))
    n = 0
    os.makedirs(dst, exist_ok=True)
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(target, f))
            n += 1
            if n % 500 == 0:
                print("\r  copied %d/%d" % (n, total), end="", flush=True)
    print("\r  copied %d/%d   " % (n, total))


def run_logged(cmd, what, logfile, cwd=None, env=None):
    """Run a command under nice, save its output to logfile, count 'missing'
    lines, and buffer stdout writes (200 lines at a time) so the terminal
    isn't doing a syscall per line for a 30-60 minute conversion."""
    full = NICE + list(cmd)
    print("  $ " + " ".join(full))
    miss = 0
    buf = []
    with open(logfile, "w", encoding="utf-8") as log:
        p = subprocess.Popen(full, cwd=cwd, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1)
        for line in p.stdout:
            buf.append(line)
            if "missing" in line.lower():
                miss += 1
            if len(buf) >= 200:
                chunk = "".join(buf)
                sys.stdout.write(chunk)
                sys.stdout.flush()
                log.write(chunk)
                buf.clear()
        if buf:
            chunk = "".join(buf)
            sys.stdout.write(chunk)
            log.write(chunk)
        p.wait()
    if p.returncode != 0:
        sys.exit("ERROR: %s failed (exit code %d) - see %s" % (what, p.returncode, logfile))
    print("  %s ok. Output in %s; %d line(s) mention 'missing'." % (what, logfile, miss))
    if miss:
        print("  Look at them:  grep -i missing %s | head -50" % logfile)


# =====================================================================
# MAIN
# =====================================================================

def main():
    ap = argparse.ArgumentParser(description="mw3ds full pipeline")
    ap.add_argument("folder", nargs="?", default=DEFAULT_PROJECT)
    ap.add_argument("--morrowind-dir", default=DEFAULT_MORROWIND)
    ap.add_argument("--makerom", default=None)
    ap.add_argument("--no-expansions", action="store_true")
    ap.add_argument("--no-convert", action="store_true")
    ap.add_argument("--convert-args", default="--world")
    ap.add_argument("--convert-only", action="store_true")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--check-sound", action="store_true")
    ap.add_argument("--pc-address", default=None)
    ap.add_argument("--no-pclink", action="store_true")
    ap.add_argument("--make", default=None)
    args = ap.parse_args()

    gc.disable()

    root = os.path.abspath(args.folder)
    print("project folder: " + root)

    if args.restore:
        restore_originals(root)
        return
    if args.check_sound:
        check_sound(args.morrowind_dir)
        return

    print("morrowind root: " + args.morrowind_dir)
    for req in ("Makefile", "tools/convert/level.py"):
        if not os.path.exists(os.path.join(root, req)):
            sys.exit("ERROR: %s not found in %s" % (req, root))
    data = os.path.join(args.morrowind_dir, "Data Files")
    if not os.path.isdir(data):
        sys.exit("ERROR: no Data Files in %s\n"
                 "  --morrowind-dir must be the install ROOT, the folder that CONTAINS Data Files"
                 % args.morrowind_dir)
    for esm in ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm"):
        if not os.path.exists(os.path.join(data, esm)):
            sys.exit("ERROR: %s not found in %s" % (esm, data))
    os.chdir(root)
    env = dict(os.environ, MW3DS_MORROWIND_DIR=args.morrowind_dir)

    if args.verify_only:
        verify_result(root, expansions=not args.no_expansions)
        return

    if not scan_tree(root, args.morrowind_dir):
        sys.exit("Fix the problems above, then rerun.")
    gc.collect()

    conv_python = None
    if not args.no_convert:
        conv_python = find_python(root)

    print("=== step 1/4: patching the converter for the expansions ===")
    if args.no_expansions:
        print("  skipped (--no-expansions)")
    elif args.no_convert:
        print("  skipped (--no-convert: the converter is not running)")
    else:
        patch_converter(root)
    gc.collect()

    if args.no_convert:
        print("=== step 2/4: conversion skipped (--no-convert): reusing out/data ===")
    else:
        print("=== step 2/4: converting the game data (30-60+ min - watch for errors) ===")
        run_logged([conv_python, "tools/convert/level.py"] + args.convert_args.split(),
                   "the conversion", os.path.join(root, "convert.log"), cwd=root, env=env)
    gc.collect()

    if not os.path.isdir(os.path.join(root, "out", "data")):
        sys.exit("ERROR: out/data is missing - run the conversion first (drop --no-convert)")
    if args.convert_only:
        print("\nStopped as asked (--convert-only). When ready: rerun with --no-convert.")
        return

    print("=== step 3/4: compiling the engine ===")
    pclink_on = False if args.no_pclink else stage_pclink(root, args.pc_address)
    if not os.environ.get("DEVKITPRO"):
        os.environ["DEVKITPRO"] = "/opt/devkitpro"
    os.environ.setdefault("DEVKITARM", os.path.join(os.environ["DEVKITPRO"], "devkitARM"))
    os.environ["PATH"] = os.pathsep.join([
        os.path.join(os.environ["DEVKITPRO"], "tools", "bin"),
        os.path.join(os.environ["DEVKITARM"], "bin"),
        os.environ.get("PATH", "")])
    make_cmd = args.make or ("make -j%d" % min(os.cpu_count() or 1, MAX_JOBS))
    run(make_cmd.split(), "the build (" + make_cmd + ")", cwd=root)
    for f in ("mw3ds.elf", "mw3ds.smdh"):
        if not os.path.exists(os.path.join(root, f)):
            sys.exit("ERROR: %s was not produced - check the make output above" % f)
    gc.collect()

    print("=== step 4/4: packaging the CIA ===")
    makerom = find_makerom(root, args.makerom)
    print("  makerom: " + makerom)
    rsf, bnr = find_build_assets(root)
    if not rsf or not bnr:
        sys.exit("ERROR: tools/build/ needs an .rsf and a prebuilt .bnr banner (found rsf=%s, bnr=%s)"
                 % (rsf, bnr))
    rsf = os.path.relpath(rsf, root)
    bnr = os.path.relpath(bnr, root)

    romfs = os.path.join(root, "build", "romfs")
    os.makedirs(romfs, exist_ok=True)
    dst = os.path.join(romfs, "data")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    print("  copying out/data into the romfs ...")
    copy_tree_streaming(os.path.join(root, "out", "data"), dst)
    if pclink_on and args.pc_address:
        with open(os.path.join(romfs, "pclink.txt"), "w") as f:
            f.write(args.pc_address.strip() + "\n")

    cia = os.path.join(root, "build", "mw3ds.cia")
    if os.path.exists(cia):
        os.remove(cia)
    try:
        run([makerom, "-f", "cia", "-o", "build/mw3ds.cia",
             "-elf", "mw3ds.elf", "-rsf", rsf,
             "-icon", "mw3ds.smdh", "-banner", bnr,
             "-exefslogo", "-target", "t", "-ignoresign",
             "-DAPP_TITLE=MW3DS", "-DAPP_PRODUCT_CODE=CTR-P-MW3D", "-DAPP_UNIQUE_ID=0xF3D50",
             "-DAPP_ROMFS=build/romfs", "-DAPP_CATEGORY=Application", "-DAPP_USE_ON_SD=true",
             "-DAPP_ENCRYPTED=false", "-DAPP_MEMORY_TYPE=Application",
             "-DAPP_SYSTEM_MODE=64MB", "-DAPP_SYSTEM_MODE_EXT=124MB",
             "-DAPP_CPU_SPEED=804MHz", "-DAPP_ENABLE_L2_CACHE=true",
             "-DAPP_VERSION_MAJOR=0"], "makerom", cwd=root)
    finally:
        shutil.rmtree(dst, ignore_errors=True)
        try:
            os.remove(os.path.join(romfs, "pclink.txt"))
        except OSError:
            pass
    if not os.path.exists(cia):
        sys.exit("ERROR: makerom produced no .cia - see its output above")

    verify_result(root, expansions=not args.no_expansions)

    print("")
    print("ALL DONE.")
    print("  .cia: %s (%.1f MiB)" % (cia, os.path.getsize(cia) / 1048576.0))
    print("")
    print("Install: copy build/mw3ds.cia to the SD card, GodMode9 -> CIA image options ->")
    print("Install game image (or FBI). Saves/settings: sdmc:/3ds/mw3ds/")


if __name__ == "__main__":
    main()
