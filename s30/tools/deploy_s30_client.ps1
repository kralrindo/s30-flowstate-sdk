# Builds (or refreshes) the S30 test client under <Workspace>\build\s30_client.
#
# Every depot file is hardlinked except r5apex_dx12.exe, which is a real copy with
# loader.dll!DummyExport added to its imports (add_import.py, the same .l2 method
# the S21 Flowstate exes use). loader.dll and client_s30.dll come from the SDK
# build output (game\). The depot itself is never written.
param(
	[string]$Workspace = "F:\apex_builds\m2299605425033557595",
	[string]$SdkGame = "$PSScriptRoot\..\..\game"
)
$ErrorActionPreference = "Stop"
$depot = Join-Path $Workspace "depot"
$client = Join-Path $Workspace "build\s30_client"
$exe = "r5apex_dx12.exe"

if (-not (Test-Path $client)) {
	Write-Host "hardlinking $depot -> $client"
	& bash -c "cp -al '$($depot -replace '\\','/')' '$($client -replace '\\','/')'"
	if ($LASTEXITCODE -ne 0) { throw "hardlink copy failed" }
}

# The exe must be a real file, never a hardlink into the depot.
$target = Join-Path $client $exe
if (Test-Path $target) { Remove-Item $target -Force }
& py (Join-Path $PSScriptRoot "add_import.py") (Join-Path $depot $exe) $target "loader.dll" "DummyExport"
if ($LASTEXITCODE -ne 0) { throw "add_import failed" }

foreach ($dll in "loader.dll", "client_s30.dll") {
	Copy-Item (Join-Path $SdkGame $dll) (Join-Path $client $dll) -Force
}
Remove-Item (Join-Path $client "s30probe.dll") -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $client "platform\logs") | Out-Null

$links = (& fsutil hardlink list (Join-Path $depot $exe)) | Measure-Object -Line
if ($links.Lines -ne 1) { throw "depot exe has extra hardlinks -- refusing" }
Write-Host "deployed: $client"
