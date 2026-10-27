#!/bin/bash
# Remote test of a vehicle seat: into the Sandbox park (to_continue.sh), the mod switched off live so the UI is in
# the screenshots, open the response facility (stops if its panel does not open: nothing is built), pick
# Ranger Team 1 (jeep) or Capture Team 1 (helicopter), switch the mod back on, DIRECT CONTROL, then print the mod's
# camera / UI-mode log lines and save <name>_seat.png. The facility is looked for in the screenshot (the park camera
# is saved with the park, and selecting things moves it, so it is not always in the middle), or given as x,y.
#   bash re/remote/vehicle.sh <name> [jeep|heli] [x,y]      HERE=1: use the park the game is in (no restart)
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
name="${1:-vehicle}"
which="${2:-jeep}"
at="${3:-}"
cd "$P"
py() { "/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "$1"; }
# A pixel of re/work/<shot>.png (960x540) as "r g b".
pixel() { py "
from PIL import Image
print(*Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('RGB').getpixel(($2, $3)))"; }
# Where the response facility is in a screenshot, as "x,y" fractions: the largest cluster of white building
# pixels below the horizon, else of grey ones, else the middle of the view.
facility() { py "
import numpy as np
from PIL import Image
im = np.asarray(Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('RGB'), dtype=np.int32)
h, w, _ = im.shape
mx, mn = im.max(2), im.min(2)
for lo, need in ((175, 20), (110, 200)):
    m = (mx - mn < 28) & (mx > lo)
    m[:int(h * 0.42)] = False; m[:, :int(w * 0.04)] = False; m[int(h * 0.95):] = False
    ys, xs = np.nonzero(m)
    if len(xs) < need: continue
    g = 16
    hist = np.zeros((h // g + 1, w // g + 1), int)
    np.add.at(hist, (ys // g, xs // g), 1)
    cy, cx = np.unravel_index(hist.argmax(), hist.shape)
    sel = (abs(ys // g - cy) <= 4) & (abs(xs // g - cx) <= 4)
    print('%.3f,%.3f' % (xs[sel].mean() / w, ys[sel].mean() / h)); break
else: print('0.5,0.52')"; }
green() { read -r r g b <<< "$(pixel "$1" "$2" "$3")"; [ "$g" -gt 220 ] && [ "$r" -lt 190 ] && [ "$b" -gt 160 ] && [ "$b" -lt 230 ]; }
mod() { sed -i "s/^Enabled = [01]/Enabled = $1/" "$D/JWE3VR.ini"; sleep 5; }

if [ -z "$HERE" ]; then
  bash re/remote/to_continue.sh "$name" 2>&1 | grep -E "game up|park after|ending|differs|stopping|lua error|dialog"
  [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "to_continue.sh failed; stopping"; exit 1; }
fi
mod 0
# Wait for the loading screen (dark blue) to give way to the park.
for i in $(seq 1 20); do
  bash re/remote/step.sh "${name}_wait" | tail -1 >/dev/null
  read -r r g b <<< "$(pixel "${name}_wait" 480 400)"
  [ "$g" -gt 80 ] && [ "$g" -gt "$b" ] && break
  sleep 5
done
# The park camera moves to its saved place a while after the park shows (seen: run 75 looked at bare grass): settle.
sleep 20
bash re/remote/step.sh "${name}_wait" | tail -1 >/dev/null
row_y=0.36; [ "$which" = "heli" ] && row_y=0.483   # Ranger Team 1 / Capture Team 1 in the facility panel
[ -n "$at" ] || at=$(facility "${name}_wait")
echo "response facility at $at"
open_team() {
  bash re/remote/step.sh "${name}_fac$1" "CLICK=$at 100" "- 400" "CLICK=$at 100" "- 3000" | tail -1
  bash re/remote/step.sh "${name}_team$1" "CLICK=0.1,$row_y 100" "- 400" "CLICK=0.1,$row_y 100" "- 3000" | tail -1
  green "${name}_team$1" 150 191
}
if ! open_team 1; then
  echo "no facility panel at $at; stopping (look at re/work/${name}_wait.png and pass the facility as x,y)"; mod 1; exit 1
fi
n0=$(wc -l < "$D/JWE3VR.log")
mod 1
bash re/remote/step.sh "${name}_dc" "CLICK=0.098,0.354 100" "- 400" "CLICK=0.098,0.354 100" "- 10000" | tail -1
sleep 15
tail -n +"$n0" "$D/JWE3VR.log" | grep -E "lua|share:|warn|error" | grep -v "camera diag" | cut -c1-260 | tail -30
bash re/remote/step.sh "${name}_seat" | tail -1
