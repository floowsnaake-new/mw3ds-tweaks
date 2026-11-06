#!/bin/bash
set -e
cd ~/Documents/Morrowind3DS_project/MW3DS
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

echo "=== clean ==="
make clean

echo "=== build ==="
make -j$(nproc)

echo "=== stage romfs ==="
rm -rf romfs
mkdir -p romfs
cp -r out/data romfs/data

echo "=== makerom ==="
./makerom -f cia -o build/mw3ds.cia \
  -elf mw3ds.elf \
  -rsf tools/build/mw3ds.rsf \
  -icon mw3ds.smdh \
  -banner tools/build/banner.bnr \
  -exefslogo -target t -ignoresign \
  -DAPP_TITLE=MW3DS \
  -DAPP_PRODUCT_CODE=CTR-P-MW3D \
  -DAPP_UNIQUE_ID=0xF3D50 \
  -DAPP_ROMFS=romfs \
  -DAPP_CATEGORY=Application \
  -DAPP_USE_ON_SD=true \
  -DAPP_ENCRYPTED=false \
  -DAPP_MEMORY_TYPE=Application \
  -DAPP_SYSTEM_MODE=64MB \
  -DAPP_SYSTEM_MODE_EXT=124MB \
  -DAPP_CPU_SPEED=804MHz \
  -DAPP_ENABLE_L2_CACHE=true \
  -DAPP_VERSION_MAJOR=0

ls -lh build/mw3ds.cia
echo "Now copy build/mw3ds.cia to the SD card and install with FBI."
