#!/bin/bash
# Remote test of leaving a park and loading it again in the same game session: into the Sandbox park
# (to_continue.sh), the mod switched off live so the UI is in the screenshots, pad START (pause menu), EXIT TO MAIN
# MENU, OK, the mod back on, CONTINUE, then the mod's log lines for the second park and a frame/HUD pair.
# Every click is gated on a screenshot check; it stops instead of clicking into an unexpected screen.
#   bash re/remote/reload.sh <name>
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
L="$D/JWE3VR.log"
name="${1:-reload}"
cd "$P"
py() { "/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "$1"; }
# What a 960x540 screenshot shows: pause (RESUME button lit), dialog (dark band, logo hidden), menu, other.
screen() { py "
import numpy as np
from PIL import Image
im = Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('RGB')
a = np.asarray(im.convert('L'), dtype=np.float32)
r, g, b = im.getpixel((420, 116))
logo, cont = a[20:110, 30:165].std(), a[175:195, 30:130].mean()
if g > 200 and b > 150 and r < 190: print('pause')
elif logo > 50 and cont > 80: print('menu')
elif logo < 10: print('dialog')
else: print('other')"; }
mod() { sed -i "s/^Enabled = [01]/Enabled = $1/" "$D/JWE3VR.ini"; sleep 5; }
stop() { echo "$1; stopping (look at re/work/$2.png)"; mod 1; exit 1; }

bash re/remote/to_continue.sh "$name" 2>&1 | grep -E "game up|park after|ending|differs|stopping|dialog|not at"
[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "to_continue.sh failed; stopping"; exit 1; }
sleep 10
mod 0
bash re/remote/step.sh "${name}_pause" "- 1500" "START 1500" "- 3000" | tail -1
[ "$(screen "${name}_pause")" = "pause" ] || stop "no pause menu" "${name}_pause"
bash re/remote/step.sh "${name}_exit" "CLICK=0.5,0.767 100" "- 400" "CLICK=0.5,0.767 100" "- 3000" | tail -1
[ "$(screen "${name}_exit")" = "dialog" ] || stop "no Exit to Main Menu dialog" "${name}_exit"
n0=$(wc -l < "$L")
bash re/remote/step.sh "${name}_ok" "CLICK=0.5,0.587 100" "- 400" "CLICK=0.5,0.587 100" "- 3000" | tail -1
sleep 25
mod 1
bash re/remote/step.sh "${name}_menu2" | tail -1
[ "$(screen "${name}_menu2")" = "menu" ] || stop "not back at the main menu" "${name}_menu2"
bash re/remote/step.sh "${name}_cont" "FOCUS+CLICK=0.079,0.343 100" "- 300" "CLICK=0.079,0.343 100" "- 500" | tail -1
start=$SECONDS
until tail -n +"$n0" "$L" | grep -q "VR manager Init" || [ $((SECONDS - start)) -gt 180 ]; do sleep 3; done
echo "second park: VR manager Init after $((SECONDS - start))s"
sleep 45
tail -n +"$n0" "$L" | grep -E "lua|warn|error|failed|in-world HUD|camera:" | grep -vE "camera diag|hooks in this VM|render-target bindings" | cut -c14-220 | tail -30
bash re/remote/hudcheck.sh "${name}_second" 2>&1 | tail -1
