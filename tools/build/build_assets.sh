#!/bin/bash
# MW3DS icon + banner builder.
# Put this script in the same folder as: icon.png  smallicon.png  banner.png  forest_audio.py
# Then run:  ./build_assets.sh
set -e
cd "$(dirname "$0")"

# ---------- EDIT THESE ----------
SRC="/home/joe/Downloads/mw3ds-main/mw3ds-main"   # your MW3DS source tree
TITLE="MW3DS"
DESC="Morrowind for New 3DS"
AUTHOR="YourName"
RUN_MAKEROM=0        # set to 1 to also rebuild the CIA at the end
ELF="$SRC/mw3ds.elf"                 # only used if RUN_MAKEROM=1
RSF="$SRC/tools/build/mw3ds.rsf"     # only used if RUN_MAKEROM=1
# --------------------------------

# 1. Check the tools
for t in smdhtool python3; do
  command -v "$t" >/dev/null || { echo "Missing: $t (smdhtool comes with devkitPro)"; exit 1; }
done
HAVE_BANNERTOOL=1
if ! command -v bannertool >/dev/null; then
  HAVE_BANNERTOOL=0
  if [ -f banner.bnr ]; then
    echo "bannertool not found, so I will reuse the banner.bnr already in this folder."
  else
    echo "Missing: bannertool and no banner.bnr here."
    echo "Build bannertool (github.com/Steveice10/bannertool) or copy a banner.bnr into this folder."
    exit 1
  fi
fi
for f in icon.png smallicon.png banner.png forest_audio.py; do
  [ -f "$f" ] || { echo "Missing file next to this script: $f"; exit 1; }
done

# 2. Generate the forest sound
echo "[1/4] Making forest audio..."
python3 forest_audio.py audio.wav

# 3. Build the icon (SMDH) and banner (BNR)
echo "[2/4] Building icon (smdh)..."
smdhtool --create "$TITLE" "$DESC" "$AUTHOR" icon.png mw3ds.smdh smallicon.png

if [ "$HAVE_BANNERTOOL" = "1" ]; then
  echo "[3/4] Building banner (bnr)..."
  bannertool makebanner -i banner.png -a audio.wav -o banner.bnr
else
  echo "[3/4] Reusing existing banner.bnr"
fi

# 4. Copy into the source tree (same places your makerom command uses)
echo "[4/4] Copying into $SRC ..."
mkdir -p "$SRC/tools/build"
cp mw3ds.smdh "$SRC/mw3ds.smdh"
cp banner.bnr "$SRC/tools/build/banner.bnr"

# Optional: rebuild the CIA
if [ "$RUN_MAKEROM" = "1" ]; then
  echo "Building CIA..."
  (cd "$SRC" && makerom -f cia -o mw3ds.cia -elf "$ELF" -rsf "$RSF" \
      -target t -exefslogo -ignoresign \
      -icon mw3ds.smdh -banner tools/build/banner.bnr)
  echo "CIA ready: $SRC/mw3ds.cia"
else
  echo "Done. Now rebuild your CIA with makerom and reinstall it with GodMode9."
fi
