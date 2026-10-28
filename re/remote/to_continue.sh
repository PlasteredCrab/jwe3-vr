#!/bin/bash
# Remote test run via the main menu's CONTINUE (a sandbox autosave exists): close (forced if in a park),
# deploy, launch, click CONTINUE, wait for the park, print the mod's Lua lines, screenshot.
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
run="${1:-run}"
taskkill //IM JWE3.exe >/dev/null 2>&1
for i in $(seq 1 15); do tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe || break; sleep 1; done
if tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe; then
  echo "still running after a close request (in a park); ending it"; taskkill //F //IM JWE3.exe >/dev/null 2>&1
  for i in $(seq 1 15); do tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe || break; sleep 1; done
fi
tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe && { echo "would not close; stopping"; exit 1; }
rm -f "$D/JWE3VR_pad.txt" "$D/JWE3VR_shot.req"
# Enabled = 0 at startup installs no hooks (no screenshots, no clicks): a script stopped mid-way can leave it off.
if grep -q "^Enabled = 0" "$D/JWE3VR.ini" 2>/dev/null; then
  echo "JWE3VR.ini had Enabled = 0 (left by a stopped run); switching it back on"
  sed -i "s/^Enabled = 0/Enabled = 1/" "$D/JWE3VR.ini"
fi
(cd "$P" && powershell -NoProfile -ExecutionPolicy Bypass -File deploy.ps1 -NoProxy 2>&1 | grep -E "installed|error")
cmp -s "$P/native/JWE3VR/build/JWE3VR.dll" "$D/JWE3VR.dll" || { echo "deploy did not replace JWE3VR.dll; stopping"; exit 1; }
old=$(head -n1 "$D/JWE3VR.log" 2>/dev/null)
"/c/Program Files (x86)/Steam/steam.exe" -applaunch 2958130 -nointro &
start=$SECONDS
until [ "$(head -n1 "$D/JWE3VR.log" 2>/dev/null)" != "$old" ] && grep -q "OpenXR session state 3\|OpenXR init failed" "$D/JWE3VR.log" 2>/dev/null || [ $((SECONDS-start)) -gt 150 ]; do sleep 3; done
if [ "$(head -n1 "$D/JWE3VR.log" 2>/dev/null)" = "$old" ]; then
  echo "the mod did not start (startup dialog?); stopping"
  powershell -NoProfile -c "Get-Process JWE3 -ErrorAction SilentlyContinue | Select-Object Id, MainWindowTitle | Format-List"; exit 1
fi
echo "game up after $((SECONDS-start))s"; sleep 20
cd "$P"
bash re/remote/step.sh "${run}_menu" "FOCUS+CLICK=0.4135,0.8315 100" "- 1000" "MOVE=0.079,0.343 1500" | grep -v "pad script" # hover CONTINUE: a cursor left on another row unlit it (run 135)
# Only click CONTINUE on the main menu itself (logo crisp, CONTINUE row lit). Run 63 opened on the "Exit game?"
# dialog instead (cause unknown); CANCEL that once and look again, else stop without clicking.
screen() { "/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "
import numpy as np
from PIL import Image
a = np.asarray(Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$1.png').convert('L'), dtype=np.float32)
logo, cont = a[20:110, 30:165].std(), a[175:195, 30:130].mean()
print('menu' if logo > 50 and cont > 80 else ('exitdialog' if logo < 10 else 'other'))"; }
s=$(screen "${run}_menu")
if [ "$s" = "exitdialog" ]; then
  echo "the Exit game dialog is open; pressing CANCEL"
  bash re/remote/step.sh "${run}_menu2" "FOCUS+CLICK=0.5,0.633 100" "- 400" "CLICK=0.5,0.633 100" "- 2000" | grep -v "pad script"
  s=$(screen "${run}_menu2")
fi
if [ "$s" != "menu" ]; then echo "not at the main menu ($s); stopping without clicking (see re/work/${run}_menu.png)"; exit 1; fi
printf '%s\n' "FOCUS+CLICK=0.079,0.343 100" "- 300" "CLICK=0.079,0.343 100" "- 500" > "$D/JWE3VR_pad.txt"
start=$SECONDS
until grep -q "VR manager Init\|lua error" "$D/JWE3VR.log" || [ $((SECONDS-start)) -gt 180 ]; do sleep 3; done
echo "park after $((SECONDS-start))s"; sleep 40
grep -v -E "hooked |marker:|debug:" "$D/JWE3VR.log" | grep -E "lua|warn|error|HUD|TAA|fove"
bash re/remote/step.sh "${run}_park" | grep -v "pad script"
cp "$D/JWE3VR.log" "re/work/${run}_JWE3VR.log"
