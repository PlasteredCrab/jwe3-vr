#!/bin/bash
# Remote test run to the main menu only: close (forced if in a park), deploy, launch, wait for the menu, screenshot
# it as re/work/<run>_menu.png. Browse on with step.sh (look at each screenshot before the next click).
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
run="${1:-run}"
taskkill //IM JWE3.exe >/dev/null 2>&1
for i in $(seq 1 15); do tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe || break; sleep 1; done
if tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe; then
  echo "still running after a close request (in a park); ending it"; taskkill //F //IM JWE3.exe >/dev/null 2>&1; sleep 3
fi
tasklist //FI "IMAGENAME eq JWE3.exe" 2>/dev/null | grep -qi jwe3.exe && { echo "would not close; stopping"; exit 1; }
rm -f "$D/JWE3VR_pad.txt" "$D/JWE3VR_shot.req"
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
bash re/remote/step.sh "${run}_menu" "FOCUS 100" "- 1000" | grep -v "pad script"
