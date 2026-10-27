#!/bin/bash
# Remote test run: close JWE3, deploy (no proxy), launch, click through to a new Sandbox "Square Levels" park,
# wait for it to load, then screenshot. Positions are fractions of the 1920x1080 client area (game 1.5.5).
#   bash re/remote/to_park.sh <run-name>
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
run="${1:-run}"
[ -f "$D/JWE3VR.log" ] && cp "$D/JWE3VR.log" "$P/re/work/prev_JWE3VR.log"
taskkill //IM JWE3.exe >/dev/null 2>&1
for i in $(seq 1 15); do tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe || break; sleep 1; done
# In a park the close request opens a quit dialog instead of exiting. Never click into it blindly: end the
# process instead (a throwaway sandbox, nothing to save), which also skips the shutdown path.
if tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe; then
  echo "JWE3 still running after a close request (in a park?); ending it"
  taskkill //F //IM JWE3.exe >/dev/null 2>&1; sleep 3
fi
if tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe; then echo "JWE3 would not close; stopping"; exit 1; fi
rm -f "$D/JWE3VR_pad.txt" "$D/JWE3VR_shot.req"
(cd "$P" && powershell -NoProfile -ExecutionPolicy Bypass -File deploy.ps1 -NoProxy 2>&1 | grep -E "installed|error")
cmp -s "$P/native/JWE3VR/build/JWE3VR.dll" "$D/JWE3VR.dll" || { echo "deploy did not replace JWE3VR.dll; stopping"; exit 1; }
old=$(head -n1 "$D/JWE3VR.log" 2>/dev/null)
"/c/Program Files (x86)/Steam/steam.exe" -applaunch 2958130 -nointro &
start=$SECONDS
until [ "$(head -n1 "$D/JWE3VR.log" 2>/dev/null)" != "$old" ] && grep -q "OpenXR session state 3\|OpenXR init failed" "$D/JWE3VR.log" 2>/dev/null || [ $((SECONDS-start)) -gt 150 ]; do sleep 3; done
if [ "$(head -n1 "$D/JWE3VR.log" 2>/dev/null)" = "$old" ]; then
  echo "the mod did not start (game stuck at a startup dialog?); stopping"
  powershell -NoProfile -c "Get-Process JWE3 -ErrorAction SilentlyContinue | Select-Object Id, MainWindowTitle | Format-List"
  exit 1
fi
echo "game up after $((SECONDS-start))s"; sleep 20
cd "$P"
bash re/remote/step.sh "${run}_menu" "FOCUS+CLICK=0.4135,0.8315 100" "- 2000"
bash re/remote/step.sh "${run}_sandbox" "FOCUS+CLICK=0.076,0.509 100" "- 300" "CLICK=0.076,0.509 100" "- 3000"
bash re/remote/step.sh "${run}_maps" "FOCUS+CLICK=0.5,0.594 100" "- 300" "CLICK=0.5,0.594 100" "- 4000"
bash re/remote/step.sh "${run}_settings" "FOCUS+CLICK=0.089,0.35 100" "- 300" "CLICK=0.089,0.35 100" "- 3000"
printf '%s\n' "FOCUS+CLICK=0.17,0.574 100" "- 300" "CLICK=0.17,0.574 100" "- 500" > "$D/JWE3VR_pad.txt"
start=$SECONDS
until grep -q "VR manager Init\|lua error" "$D/JWE3VR.log" || [ $((SECONDS-start)) -gt 150 ]; do sleep 3; done
echo "park after $((SECONDS-start))s"; sleep 40
bash re/remote/step.sh "${run}_park"
cp "$D/JWE3VR.log" "re/work/${run}_JWE3VR.log"
grep -v -E "hooked |marker:|debug:" "$D/JWE3VR.log" | grep -E "lua|warn|error|HUD|TAA|fove"
