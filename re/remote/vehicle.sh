#!/bin/bash
# Remote test of a vehicle seat: into the Sandbox park (to_continue.sh), the mod switched off live so the UI is in
# the screenshots, open the response facility in the middle of the view (built first if it is not there), pick
# Ranger Team 1 (jeep) or Capture Team 1 (helicopter), switch the mod back on, DIRECT CONTROL, then print the mod's
# camera / UI-mode log lines and save <name>_seat.png.
#   bash re/remote/vehicle.sh <name> [jeep|heli]
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
name="${1:-vehicle}"
which="${2:-jeep}"
cd "$P"
py() { "/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "$1"; }
# A pixel of re/work/<shot>.png (960x540) as "r g b".
pixel() { py "
from PIL import Image
print(*Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('RGB').getpixel(($2, $3)))"; }
green() { read -r r g b <<< "$(pixel "$1" "$2" "$3")"; [ "$g" -gt 220 ] && [ "$r" -lt 190 ] && [ "$b" -gt 160 ] && [ "$b" -lt 230 ]; }
mod() { sed -i "s/^Enabled = [01]/Enabled = $1/" "$D/JWE3VR.ini"; sleep 5; }

bash re/remote/to_continue.sh "$name" 2>&1 | grep -E "game up|park after|ending|differs|stopping|lua error"
mod 0
# Wait for the loading screen (dark blue) to give way to the park.
for i in $(seq 1 20); do
  bash re/remote/step.sh "${name}_wait" | tail -1 >/dev/null
  read -r r g b <<< "$(pixel "${name}_wait" 480 400)"
  [ "$g" -gt 80 ] && [ "$g" -gt "$b" ] && break
  sleep 5
done
row_y=0.36; [ "$which" = "heli" ] && row_y=0.483   # Ranger Team 1 / Capture Team 1 in the facility panel
open_team() {
  bash re/remote/step.sh "${name}_fac$1" "CLICK=0.5,0.52 100" "- 400" "CLICK=0.5,0.52 100" "- 3000" | tail -1
  bash re/remote/step.sh "${name}_team$1" "CLICK=0.1,$row_y 100" "- 400" "CLICK=0.1,$row_y 100" "- 3000" | tail -1
  green "${name}_team$1" 150 191
}
if ! open_team 1; then
  echo "no facility panel; building a response facility"
  bash re/remote/step.sh "${name}_ops" "CLICK=0.0125,0.156 100" "- 400" "CLICK=0.0125,0.156 100" "- 2500" | tail -1
  bash re/remote/step.sh "${name}_rs" "CLICK=0.0646,0.25 100" "- 400" "CLICK=0.0646,0.25 100" "- 2500" | tail -1
  bash re/remote/step.sh "${name}_place" "CLICK=0.5,0.52 100" "- 1500" "CLICK=0.5,0.52 100" "- 3000" | tail -1
  bash re/remote/step.sh "${name}_closeinfo" "CLICK=0.98,0.98 100" "- 1500" | tail -1
  if ! open_team 2; then
    echo "team panel not found; stopping (look at re/work/${name}_team2.png)"; mod 1; exit 1
  fi
fi
n0=$(wc -l < "$D/JWE3VR.log")
mod 1
bash re/remote/step.sh "${name}_dc" "CLICK=0.098,0.354 100" "- 400" "CLICK=0.098,0.354 100" "- 10000" | tail -1
sleep 15
tail -n +"$n0" "$D/JWE3VR.log" | grep -E "lua|share:|warn|error" | grep -v "camera diag" | cut -c1-260 | tail -30
bash re/remote/step.sh "${name}_seat" | tail -1
