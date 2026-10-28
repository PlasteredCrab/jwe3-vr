#!/bin/bash
# Remote-test helper like step.sh, one layer up: plays a VR controller script (JWE3VR_vr.txt, format in xr.cpp), so
# the mod's own controller mapping runs as with real controllers, then saves a screenshot as re/work/<name>.png.
#   bash re/remote/vrstep.sh <name> [script lines...]     e.g. bash re/remote/vrstep.sh v1 "- 1000" "R.CLICK 1500"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
name="$1"; shift
if [ $# -gt 0 ]; then
  printf '%s\n' "$@" > "$D/JWE3VR_vr.txt"
  start=$SECONDS
  until ! [ -f "$D/JWE3VR_vr.txt" ] || [ $((SECONDS-start)) -gt 15 ]; do sleep 0.5; done
  [ -f "$D/JWE3VR_vr.txt" ] && echo "VR script NOT picked up" && rm -f "$D/JWE3VR_vr.txt"
  total=0; for l in "$@"; do ms=${l##* }; total=$((total+ms)); done
  sleep $(( (total+999)/1000 + 1 ))
fi
bash "$(dirname "$0")/step.sh" "$name" | tail -1
