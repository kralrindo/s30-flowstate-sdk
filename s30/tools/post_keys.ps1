# Posts keyboard messages to the S30 test client's game window (class Respawn001,
# owned by a process running from -ClientDir). Exercises the overlay's WndProc
# path the way a real key press would, without needing input focus.
#   post_keys.ps1 -Key F10                 toggle the overlay
#   post_keys.ps1 -Text "sdk_status" -Enter type into the focused overlay input
param(
	[string]$ClientDir = "F:\apex_builds\m2299605425033557595\build\s30_client",
	[string]$Key = "",
	[string]$Text = "",
	[switch]$Enter
)
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class S30Win {
	public delegate bool EnumProc(IntPtr h, IntPtr l);
	[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
	[DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
	[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@
$pids = @(Get-CimInstance Win32_Process -Filter "Name='r5apex_dx12.exe'" | Where-Object { $_.ExecutablePath -like "$ClientDir*" } | ForEach-Object { [uint32]$_.ProcessId })
if (-not $pids) { throw "test client not running" }
$script:hwnd = [IntPtr]::Zero
[S30Win]::EnumWindows({ param($h, $l)
	$sb = New-Object System.Text.StringBuilder 64
	[void][S30Win]::GetClassName($h, $sb, 64)
	$p = [uint32]0; [void][S30Win]::GetWindowThreadProcessId($h, [ref]$p)
	if ($sb.ToString() -eq "Respawn001" -and $pids -contains $p) { $script:hwnd = $h; return $false }
	return $true }, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw "game window not found" }

$WM_KEYDOWN = 0x100; $WM_KEYUP = 0x101; $WM_CHAR = 0x102
function Send-Key([int]$vk) {
	[void][S30Win]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]1)
	Start-Sleep -Milliseconds 40
	[void][S30Win]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]$vk, [IntPtr]0xC0000001)
	Start-Sleep -Milliseconds 40
}
$vkMap = @{ "F10" = 0x79; "ENTER" = 0x0D; "ESCAPE" = 0x1B; "TAB" = 0x09; "UP" = 0x26; "DOWN" = 0x28; "BACKTICK" = 0xC0 }
if ($Key) { Send-Key $vkMap[$Key.ToUpper()] }
foreach ($c in $Text.ToCharArray()) {
	[void][S30Win]::PostMessage($script:hwnd, $WM_CHAR, [IntPtr][int]$c, [IntPtr]1)
	Start-Sleep -Milliseconds 25
}
if ($Enter) { Send-Key 0x0D }
"posted to window 0x{0:X}" -f $script:hwnd.ToInt64()
