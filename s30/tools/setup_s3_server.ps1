# Builds the workspace S3 dedicated server (build\s3_server) from the S21 Flowstate
# install, read-only: C:\R5Flowstate is never written. Then swaps in what the S30
# client needs the server to match:
#   net_prophuff.dat -- the netprop Huffman table must be identical on both ends.
#                       S30 ships it as a wrap asset in common_early.rpak
#                       (0x323db47102530468); it differs from S21's in its last byte.
#   setup_s3_server.ps1 [-Maps mp_rr_canyonlands_staging_mu1,mp_rr_freedm_skulltown]
param(
	[string]$Source = "C:\R5Flowstate",
	[string]$Workspace = "F:\apex_builds\m2299605425033557595",
	[string[]]$Maps = @("mp_rr_canyonlands_staging_mu1", "mp_rr_freedm_skulltown"),
	[string]$Rsx = "C:\Users\Lorry\Documents\apex\tools\rsx\bin\Release\rsx.exe"
)
$ErrorActionPreference = "Stop"
$dst = Join-Path $Workspace "build\s3_server"
New-Item -ItemType Directory -Force $dst | Out-Null

# Root files. r5apexdata.bin is required: the dedi's search-path setup calls Error("")
# without it (FileSystem_LoadSearchPaths, r5apex_ds 0x210F70).
foreach ($f in "r5apex_ds.exe", "server.dll", "server.pdb", "loader.dll", "gameinfo.txt", "build.txt", "gameversion.txt", "r5apexdata.bin") {
	Copy-Item (Join-Path $Source $f) $dst -Force
}
foreach ($d in "cfg", "bin", "r2", "vpk", "paks\Win64_server") {
	robocopy (Join-Path $Source $d) (Join-Path $dst $d) /E /NFL /NDL /NJH /NJS /NP | Out-Null
}
robocopy (Join-Path $Source "platform") (Join-Path $dst "platform") /E /XD logs demos /NFL /NDL /NJH /NJS /NP | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dst "maps\navmesh") | Out-Null
foreach ($m in $Maps) {
	Get-ChildItem (Join-Path $Source "maps\navmesh") -Filter "$m*.nm" | Copy-Item -Destination (Join-Path $dst "maps\navmesh") -Force
}

# S30 netprop Huffman table.
$export = Join-Path $Workspace "assets_converted\s30_common_early_wrap"
& $Rsx -nogui -nocachedb -export -exportfullpaths --exporttypes wrap --exportdir $export (Join-Path $Workspace "depot\paks\Win64\common_early.rpak") | Out-Null
$huff = Join-Path $export "wrap\net_prophuff.dat"
if (-not (Test-Path $huff) -or (Get-Item $huff).Length -ne 16384) { throw "S30 net_prophuff.dat export missing or wrong size" }
$platform = Join-Path $dst "platform"
if (-not (Test-Path "$platform\net_prophuff.dat.s21")) { Copy-Item "$platform\net_prophuff.dat" "$platform\net_prophuff.dat.s21" }
Copy-Item $huff "$platform\net_prophuff.dat" -Force
Remove-Item "$platform\veh_crash.log" -ErrorAction SilentlyContinue
"S3 dedi ready at $dst (S30 net_prophuff.dat installed)"
