#!/bin/bash
# Remote test of walking (the guest view): into the Sandbox park (to_continue.sh), the mod switched off live so the
# UI is in the screenshots, the guest-camera button in the left toolbar, a start spot in the view, the mod back on,
# then the mod's log lines and <name>_guest.png. Stops instead of clicking on when a screen is not what it expects.
#   bash re/remote/guest.sh <name> [x,y]      x,y = the start spot (default: the middle of the view)
#   HERE=1: use the park the game is in (no restart)
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
L="$D/JWE3VR.log"
name="${1:-guest}"
at="${2:-0.5,0.6}"
cd "$P"
py() { "/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "$1"; }
pixel() { py "
from PIL import Image
print(*Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('RGB').getpixel(($2, $3)))"; }
# The dropper's "SELECT START LOCATION" label: a dark box in the middle of the view with light text in it.
dropper() { py "
import numpy as np
from PIL import Image
a = np.asarray(Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('L'), dtype=np.float32)
box = a[272:290, 500:600]
print('yes' if box.mean() < 90 and box.max() > 180 else 'no')"; }
mod() { sed -i "s/^Enabled = [01]/Enabled = $1/" "$D/JWE3VR.ini"; sleep 5; }
stop() { echo "$1; stopping (look at re/work/$2.png)"; mod 1; exit 1; }

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
# The guest-camera button (a person with a video camera), second from the bottom of the left toolbar.
bash re/remote/step.sh "${name}_dropper" "CLICK=0.0125,0.581 100" "- 400" "CLICK=0.0125,0.581 100" "- 3000" | tail -1
[ "$(dropper "${name}_dropper")" = "yes" ] || stop "no SELECT START LOCATION" "${name}_dropper"
n0=$(wc -l < "$L")
bash re/remote/step.sh "${name}_drop" "CLICK=$at 100" "- 400" "CLICK=$at 100" "- 8000" | tail -1
start=$SECONDS
until tail -n +"$n0" "$L" | grep -q "ui mode guest in" || [ $((SECONDS - start)) -gt 30 ]; do sleep 2; done
tail -n +"$n0" "$L" | grep -q "ui mode guest in" || stop "no guest view (start spot obstructed or outside the park?)" "${name}_drop"
bash re/remote/step.sh "${name}_flat" | tail -1
mod 1
sleep 5
tail -n +"$n0" "$L" | grep -E "lua|warn|error" | grep -v "camera diag" | cut -c1-220 | tail -12
bash re/remote/step.sh "${name}_guest" | tail -1
