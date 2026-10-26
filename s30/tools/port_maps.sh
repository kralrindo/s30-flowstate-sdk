#!/bin/bash
# docs/22: S30 maps -> the map-port test dedi, one map at a time (map pak, server BSP VPK,
# install into build/s3_server_mapport, load test). One map failing does not stop the rest.
#   bash port_maps.sh <map> [<map> ...]      results: asset_pipeline/s30_dedi_maps/port_maps.log
WS=/f/apex_builds/m2299605425033557595
T=$WS/sdk_port/r5sdk/s30/tools
TEST=$WS/build/s3_server_mapport
LOG=$WS/asset_pipeline/s30_dedi_maps/port_maps.log
mkdir -p "$(dirname "$LOG")"
for m in "$@"; do
	out=$WS/asset_pipeline/s30_dedi_maps/$m.port.txt
	{
		echo "######## $m $(date '+%F %T')"
		py "$T/dedi_paks.py" --map "$m" --server "$TEST" list export models anims buildlist repak install && pak=ok || pak=FAILED
		echo "pak: $pak"
		if [ $pak = ok ]; then
			py "$T/dedi_map.py" "$m" --server "$TEST" export stage convert closure gate vpk install && bsp=ok || bsp=FAILED
		else
			bsp=skipped
		fi
		echo "bsp: $bsp"
		if [ $bsp = ok ]; then
			powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$T/dedi_map_test.ps1")" -Map "$m" -Seconds 600 | grep -v "Music Pack Asset"
		fi
	} > "$out" 2>&1
	{
		echo "$m: pak $pak, bsp $bsp, $(grep -m1 '^RESULT' "$out" || echo 'no test')"
		grep -m3 "^closure:\|unregistered\|problems" "$out"
	} >> "$LOG"
done
echo "done" >> "$LOG"
