# Starts the S3 dedicated server from the workspace copy (build\s3_server; never
# C:\R5Flowstate) and waits until the Flowstate host reports ready.
#   start_s3_server.ps1 [-Playlist survival_firingrange] [-Port 37015] [-Seconds 240]
# Only dedis running from ServerDir are stopped first.
param(
	[string]$ServerDir = "F:\apex_builds\m2299605425033557595\build\s3_server",
	[string]$Playlist = "survival_firingrange",
	[int]$Port = 37015,
	[int]$Seconds = 240
)
$ErrorActionPreference = "Stop"
Get-CimInstance Win32_Process -Filter "Name='r5apex_ds.exe'" | Where-Object { $_.ExecutablePath -like "$ServerDir*" } |
	ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Sleep 1

$args = "-dedicated -playlistFile `"playlists_r5_patch.txt`" -port $Port -devsdk -dev +sv_cheats 1 " +
	"+sv_allowSendTableTransmitToClients 1 +stringtable_compress 1 +launchplaylist $Playlist"
$p = Start-Process -FilePath "$ServerDir\r5apex_ds.exe" -WorkingDirectory $ServerDir -ArgumentList $args -PassThru -WindowStyle Minimized
"dedi pid $($p.Id) port $Port playlist $Playlist"
Start-Sleep 5
$logDir = (Get-ChildItem "$ServerDir\platform\logs\server" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
"log: $logDir"

$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
	if ($p.HasExited) { "server exited with code $($p.ExitCode)"; break }
	if (Select-String -Path "$logDir\*.log" -Pattern "R5F-HOST\] ready" -Quiet -ErrorAction SilentlyContinue) { "READY"; break }
	Start-Sleep 3
}
Select-String -Path "$logDir\*.log" -Pattern "R5F-HOST\]|Error|error" -ErrorAction SilentlyContinue | Select-Object -Last 8 | ForEach-Object { $_.Line }
