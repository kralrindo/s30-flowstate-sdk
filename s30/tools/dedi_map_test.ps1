# Map load test on the map-port test dedi (docs/22): starts build\s3_server_mapport's
# r5apex_ds.exe on its own port with +map <map>, waits for "[R5F-HOST] ready <map>",
# watches for modal dialogs (missing model "SDK Error", Error()), summarises the run's
# error/script logs, then stops that dedi. Only dedis running from $ServerDir are touched.
#   dedi_map_test.ps1 -Map mp_rr_thunderdome [-Port 37016] [-Seconds 420]
param(
	[Parameter(Mandatory = $true)][string]$Map,
	[string]$ServerDir = "F:\apex_builds\m2299605425033557595\build\s3_server_mapport",
	[int]$Port = 37016,
	[int]$Seconds = 420
)
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class MapTestWin {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static List<string> Dialogs(uint pid) {
    var r = new List<string>();
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p != pid || !IsWindowVisible(h)) return true;
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() != "#32770") return true;
      var t = new StringBuilder(512); GetWindowText(h, t, 512);
      var body = new StringBuilder();
      EnumChildWindows(h, (ch, l2) => { var s = new StringBuilder(2048); GetWindowText(ch, s, 2048); if (s.Length > 0) body.Append(s).Append(" | "); return true; }, IntPtr.Zero);
      r.Add(t + ": " + body);
      return true; }, IntPtr.Zero);
    return r;
  }
}
"@
function Stop-TestDedi {
	Get-CimInstance Win32_Process -Filter "Name='r5apex_ds.exe'" | Where-Object { $_.ExecutablePath -like "$ServerDir\*" } |
		ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
}
Stop-TestDedi
Start-Sleep 1
$logRoot = "$ServerDir\platform\logs\server"
New-Item -ItemType Directory -Force $logRoot | Out-Null
$before = @(Get-ChildItem $logRoot -Directory | ForEach-Object Name)
$env:VPROJECT = "1"
$a = "-dedicated -playlistFile `"playlists_r5_patch.txt`" -port $Port -devsdk -dev +sv_cheats 1 " +
	"+sv_allowSendTableTransmitToClients 1 +stringtable_compress 1 +sdk_pak_load_flowstate 0 +bridge_client_set s30 +map $Map"
$p = Start-Process -FilePath "$ServerDir\r5apex_ds.exe" -WorkingDirectory $ServerDir -ArgumentList $a -PassThru -WindowStyle Minimized
$result = "timeout"; $dialogs = @(); $dir = $null
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
	Start-Sleep 2
	if (-not $dir) {
		$new = Get-ChildItem $logRoot -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
		if ($new) { $dir = $new.FullName }
	}
	if ($p.HasExited) { $result = "exited (code $($p.ExitCode))"; break }
	$dialogs = [MapTestWin]::Dialogs([uint32]$p.Id)
	if ($dialogs.Count) { $result = "dialog"; break }
	if ($dir -and (Select-String -Path "$dir\message.log" -Pattern "\[R5F-HOST\] ready $Map" -Quiet -ErrorAction SilentlyContinue)) {
		Start-Sleep 8   # let level-start scripts and the first frames run
		if ($p.HasExited) { $result = "ready, then exited (code $($p.ExitCode))"; break }
		$dialogs = [MapTestWin]::Dialogs([uint32]$p.Id)
		$result = if ($dialogs.Count) { "ready+dialog" } else { "ready" }
		break
	}
}
$secs = [int]((Get-Date) - $t0).TotalSeconds
"RESULT $Map : $result after ${secs}s (log $dir)"
$dialogs | ForEach-Object { "DIALOG $_" }
if ($dir) {
	foreach ($f in "error.log", "script_warning.log") {
		if (Test-Path "$dir\$f") {
			$lines = Get-Content "$dir\$f" | Where-Object { $_ -notmatch "not found; replacing with" }
			"--- $f : $($lines.Count) lines"
			$lines | Select-Object -First 15
		}
	}
	"--- script errors"
	Select-String -Path "$dir\*.log" -Pattern "SCRIPT ERROR|SCRIPT COMPILE ERROR|\[pak-fixup\] unresolved|Error\(\)|Host_Error|missing model" -ErrorAction SilentlyContinue |
		Select-Object -First 15 | ForEach-Object { $_.Line }
	$miss = @(Select-String -Path "$dir\error.log" -Pattern "not found; replacing with" -ErrorAction SilentlyContinue)
	"--- missing models: $($miss.Count)"
	$miss | Select-Object -First 10 | ForEach-Object { $_.Line }
}
Stop-TestDedi
