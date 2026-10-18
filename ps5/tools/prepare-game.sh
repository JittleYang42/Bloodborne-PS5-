#!/usr/bin/env bash
# bbport PS5 - the game-side files for the console title, made on the PC from a working
# Windows/Linux bbport install (whose launcher has prepared out/ for your CUSA03173 1.09 dump).
#
#   prepare-game.sh BBPORT_DIR TITLE_DIR
#
# BBPORT_DIR  the PC port: bbport.ini, scripts/, patches/, out/boot-linked.bin, out/content.bin,
#             out/eboot.elf (start the game once from the PC launcher to make them)
# TITLE_DIR   the built title folder (dist/PPSA99173): its bbport/ folder is filled
#
# Writes bbport/boot-linked.bin, content.bin, patches-60.bin, patches-uncap.bin (the scene at
# 1916x1078, FSR 3.1 to 3840x2160, Quit to Desktop off: a title cannot quit to the home screen
# that way), bbport.ini and input.ini. The game dump itself is copied to the console separately
# (README-PS5.txt).
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
port=$(cd -- "$1" && pwd)
title=$(cd -- "$2" && pwd)
render=${BB_PS5_RENDER_RES:-1916x1078}
output=${BB_PS5_OUTPUT_RES:-3840x2160}
for file in out/boot-linked.bin out/content.bin out/eboot.elf scripts/patches.py patches/Bloodborne.xml bbport.ini; do
    [[ -f $port/$file ]] || { echo "missing $port/$file: start the game once from the PC launcher" >&2; exit 2; }
done
work="$port/out-ps5"
mkdir -p "$work" "$title/bbport"
ln -f "$port/out/eboot.elf" "$work/eboot.elf" 2>/dev/null || cp "$port/out/eboot.elf" "$work/eboot.elf"
sed -e 's/^upscaler=.*/upscaler=fsr3/' -e 's/^preset=.*/preset=3/' -e "s/^output_res=.*/output_res=$output/" \
    -e 's/^model_lod=.*/model_lod=0/' -e 's/^quit_to_desktop=.*/quit_to_desktop=0/' \
    -e 's/^live_resolution=.*/live_resolution=0/' -e 's/^frame_gen=.*/frame_gen=off/' \
    "$port/bbport.ini" > "$title/bbport/bbport.ini"
for fps in 60 uncap; do
    (cd "$port" && BB_QUIT_TO_DESKTOP=0 python3 scripts/patches.py --out "$work" --fps "$fps" \
        --settings "$title/bbport/bbport.ini" --game-dir CUSA03173 --render-res "$render" \
        --output-res "$output" --xml patches/Bloodborne.xml)
    mv "$work/patches.bin" "$title/bbport/patches-$fps.bin"
done
cp "$port/out/boot-linked.bin" "$port/out/content.bin" "$title/bbport/"
[[ -f $port/input.ini ]] && cp "$port/input.ini" "$title/bbport/input.ini"
echo "==> $title/bbport ready (scene $render, output $output)"
