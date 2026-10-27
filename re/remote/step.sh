#!/bin/bash
# Remote-test helper: optionally plays a pad script, then saves a screenshot as re/work/<name>.png (960x540).
#   bash re/remote/step.sh <name> [script lines...]     e.g. bash re/remote/step.sh s3 "A 150" "- 1500"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
W="/e/Modding/Jurassic Park Evolution 3 VR/re/work"
name="$1"; shift
if [ $# -gt 0 ]; then
  printf '%s\n' "$@" > "$D/JWE3VR_pad.txt"
  start=$SECONDS
  until ! [ -f "$D/JWE3VR_pad.txt" ] || [ $((SECONDS-start)) -gt 15 ]; do sleep 0.5; done
  [ -f "$D/JWE3VR_pad.txt" ] && echo "pad script NOT picked up" && rm -f "$D/JWE3VR_pad.txt"
  total=0; for l in "$@"; do ms=${l##* }; total=$((total+ms)); done
  sleep $(( (total+999)/1000 + 1 ))
fi
rm -f "$D/JWE3VR_shot.bmp"; touch "$D/JWE3VR_shot.req"
start=$SECONDS
until [ -f "$D/JWE3VR_shot.bmp" ] || [ $((SECONDS-start)) -gt 20 ]; do sleep 0.5; done
sleep 1
grep "debug:" "$D/JWE3VR.log" | tail -3
"/e/Modding/tools/cobra-tools/.venv/Scripts/python" -E -c "
from PIL import Image
Image.open(r'E:/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR/JWE3VR_shot.bmp').resize((960, 540)).save(r'E:/Modding/Jurassic Park Evolution 3 VR/re/work/$name.png')
print('saved $name.png')"
