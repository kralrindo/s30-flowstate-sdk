# docs/22: copy maps verified on the map-port test dedi (build\s3_server_mapport) into the
# workspace dedi (build\s3_server). Per map: paks\Win64_server\<map>.rpak and the server BSP
# VPK pair; the mp_rr_canyonlands_hu_avt maps also need the crypto holo map script fix.
# A replaced file is kept as <file>.s21 (when none exists yet) and is never written through
# a hard link (the test dedi shares its paks/vpk with the workspace one).
#   install_maps.ps1 [-Maps mp_rr_thunderdome,mp_rr_olympus_mu3]   (default: every ready map)
param(
	[string[]]$Maps = @("mp_rr_thunderdome", "mp_rr_freedm_skulltown", "mp_quicklighting_test",
		"mp_rr_district_mu1", "mp_rr_district_mu1_uh", "mp_rr_divided_moon_mu1", "mp_rr_divided_moon_mu1_uh",
		"mp_rr_olympus_mu3", "mp_rr_olympus_mu3_uh", "mp_rr_olympus_mu3_uh_snapback", "mp_rr_desertlands_mu5",
		"mp_rr_tropic_island_mu2_landscape", "mp_rr_canyonlands_hu_avt", "mp_rr_canyonlands_hu_avt_uh",
		"mp_rr_canyonlands_hu_avt_uh_night"),
	[string]$From = "F:\apex_builds\m2299605425033557595\build\s3_server_mapport",
	[string]$To = "F:\apex_builds\m2299605425033557595\build\s3_server"
)
$ErrorActionPreference = "Stop"
if (Get-CimInstance Win32_Process -Filter "Name='r5apex_ds.exe'" | Where-Object { $_.ExecutablePath -like "$To\*" }) {
	throw "a dedi is running from $To -- stop it first"
}
function Install-File([string]$rel) {
	$src = Join-Path $From $rel
	$dst = Join-Path $To $rel
	if (-not (Test-Path $src)) { throw "missing $src" }
	if ((Test-Path $dst) -and -not (Test-Path "$dst.s21")) { Move-Item $dst "$dst.s21" }
	if (Test-Path $dst) { Remove-Item $dst -Force }   # never write through a hard link
	New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
	Copy-Item $src $dst
	"installed $rel"
}
foreach ($m in $Maps) {
	Install-File "paks\Win64_server\$m.rpak"
	Install-File "vpk\englishserver_$m.bsp.pak000_dir.vpk"
	Install-File "vpk\server_$m.bsp.pak000_000.vpk"
}
if ($Maps | Where-Object { $_ -like "mp_rr_canyonlands_hu_avt*" }) {
	Install-File "platform\scripts\vscripts\mp\levels\sh_crypto_tt_common.nut"
}
