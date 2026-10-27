#!/bin/bash
# In-world HUD check in a running park: dumps the HUD texture (JWE3VR_hud.req) and puts it next to a fresh
# backbuffer screenshot in re/work/<name>_pair.png (left: what the eyes get, right: what went to the HUD panel).
#   bash re/remote/hudcheck.sh <name>
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
name="${1:-hud}"
cd "$P"
bash re/remote/step.sh "${name}_frame" | grep -v "pad script"
rm -f "$D/JWE3VR_hud.bmp"; touch "$D/JWE3VR_hud.req"
for i in $(seq 1 40); do [ -f "$D/JWE3VR_hud.bmp" ] && break; sleep 1; done
sleep 1
grep -E "in-world HUD|JWE3VR_hud|no HUD|dump skipped" "$D/JWE3VR.log" | tail -6
"/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "
from PIL import Image
a = Image.open(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/${name}_frame.png').resize((480, 270))
try:
    b = Image.open(r'E:/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR/JWE3VR_hud.bmp').resize((480, 270))
except Exception as e:
    b = Image.new('RGB', (480, 270), 'red'); print('no hud dump:', e)
s = Image.new('RGB', (960, 270)); s.paste(a, (0, 0)); s.paste(b, (480, 0))
s.save(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/${name}_pair.png'); print('saved ${name}_pair.png')"
