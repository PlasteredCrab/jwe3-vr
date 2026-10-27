#!/bin/bash
# Remote test of a vehicle seat without the response facility's panel: into the Sandbox park (to_continue.sh), then
# JWE3VR_drive.req makes the mod take control of the ranger jeep or ACU helicopter nearest the camera (the radio's
# and staff wand's own vehicle finder), then the mod's log lines and <name>_seat.png. No clicks.
#   bash re/remote/drive.sh <name> [jeep|heli]      HERE=1: use the park the game is in (no restart)
P="/e/Modding/Jurassic Park Evolution 3 VR"
D="/e/SteamLibrary/steamapps/common/Jurassic World Evolution 3/Win64/ovldata/JWE3VR"
L="$D/JWE3VR.log"
name="${1:-drive}"
which="${2:-jeep}"
cd "$P"
if [ -z "$HERE" ]; then
  bash re/remote/to_continue.sh "$name" 2>&1 | grep -E "game up|park after|ending|differs|stopping|lua error|dialog|Enabled"
  [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "to_continue.sh failed; stopping"; exit 1; }
  sleep 20
fi
n0=$(wc -l < "$L")
echo "$which" > "$D/JWE3VR_drive.req"
start=$SECONDS
until tail -n +"$n0" "$L" | grep -q "ui mode drive in\|debug drive: nearest .* is nil\|debug drive failed" || [ $((SECONDS - start)) -gt 60 ]; do
  sleep 2
done
sleep 10
tail -n +"$n0" "$L" | grep -E "lua|debug drive|drive request|warn|error" | grep -v "camera diag" | cut -c1-220 | tail -12
tail -n +"$n0" "$L" | grep -q "ui mode drive in" || { echo "not in the vehicle; stopping"; exit 1; }
bash re/remote/step.sh "${name}_seat" | tail -1
