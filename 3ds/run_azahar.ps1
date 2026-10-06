<#
run_azahar.ps1 - run build3ds/ac_3ds.3dsx in Azahar, wait for a log line, then close Azahar.

  powershell -File 3ds/run_azahar.ps1 [-Until <regex>] [-Timeout <s>] [-Capture] [-Gdb]

Stops at the first of: a log line that matches -Until, a [CRASH] line, an Azahar CPU
exception dialog, Azahar exit,
no game log output for -Stall seconds, or -Timeout seconds. Azahar is always force-killed at the end, so the script cannot hang.
Output: build3ds/last_log.txt (game log), build3ds/last_shot.png (-Capture), and
[CRASH] addresses resolved to source lines (needs Docker and the ac3ds-work volume).
-Gdb: start with the Azahar GDB stub on port 24689. Azahar then waits for a debugger
(see 3ds/gdb.sh) and -Timeout still applies.
#>
param(
    [string]$Until = "",
    [int]$Timeout = 60,
    [int]$Stall = 10,
    [switch]$Capture,
    [switch]$Gdb
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sd = "$env:APPDATA\Azahar\sdmc\3ds\AnimalCrossing"
$cfg = "$env:APPDATA\Azahar\config\qt-config.ini"
$exe = "C:\Program Files\Azahar\azahar.exe"
$log = "$sd\log.txt"
$emuLog = "$env:APPDATA\Azahar\logzahar_log.txt"
$out = "$repo\build3ds"

function Kill-Azahar {
    cmd /c "taskkill /F /T /IM azahar.exe >nul 2>&1"
    for ($i = 0; $i -lt 50 -and (Get-Process azahar -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 100
    }
}

# Last $max bytes of a file that Azahar keeps open (the emulator log can reach 100 MB)
function Read-Tail([string]$path, [int]$max = 65536) {
    try {
        $fs = [IO.File]::Open($path, "Open", "Read", "ReadWrite")
        if ($fs.Length -gt $max) { $fs.Seek(-$max, "End") | Out-Null }
        $sr = New-Object IO.StreamReader($fs)
        $t = $sr.ReadToEnd()
        $sr.Close()
        return $t
    } catch { return "" }
}

function Set-GdbStub([bool]$on) {
    $v = if ($on) { "true" } else { "false" }
    $text = Get-Content $cfg -Raw
    # instant_debug_log: Azahar flushes each log line, so an exception dump shows at once
    foreach ($kv in @("use_gdbstub\default=false", "use_gdbstub=$v", "gdbstub_port\default=false", "gdbstub_port=24689",
                      "instant_debug_log\default=false", "instant_debug_log=true")) {
        $key = [regex]::Escape(($kv -split "=")[0])
        if ($text -match "(?m)^$key=") { $text = $text -replace "(?m)^$key=.*$", $kv }
        else { $text = $text -replace "(?m)^\[Debugging\]\r?$", "[Debugging]`r`n$kv" }
    }
    Set-Content $cfg $text -NoNewline
}

Kill-Azahar
Copy-Item "$out\ac_3ds.3dsx" "$sd\ac_3ds.3dsx" -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item $emuLog -ErrorAction SilentlyContinue # an old exception dump must not stop this run
Set-GdbStub $Gdb.IsPresent

# Visible top-level windows of a process: Azahar's error dialog is a second one
Add-Type @"
using System; using System.Runtime.InteropServices;
public class AzEnum {
  delegate bool CB(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(CB cb, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  public static int Count(uint pid) {
    int n = 0;
    EnumWindows((h, p) => { uint q; GetWindowThreadProcessId(h, out q); if (q == pid && IsWindowVisible(h)) n++; return true; }, IntPtr.Zero);
    return n;
  }
}
"@

$p = Start-Process $exe -ArgumentList "`"$sd\ac_3ds.3dsx`"" -PassThru
$start = Get-Date
$reason = "timeout ($Timeout s)"
$lastSize = -1
$lastGrow = Get-Date
try {
    while (((Get-Date) - $start).TotalSeconds -lt $Timeout) {
        Start-Sleep -Milliseconds 500
        if ($p.HasExited) { $reason = "Azahar exited"; break }
        # Azahar shows a dialog on a guest CPU exception and the game stops: kill at once
        if ([AzEnum]::Count([uint32]$p.Id) -gt 1) { Start-Sleep -Milliseconds 300; $reason = "Azahar dialog (exception or error)"; break }
        if ((Read-Tail $emuLog) -match "Exception Type:") { Start-Sleep -Milliseconds 500; $reason = "emulator exception"; break }
        if (-not (Test-Path $log)) { continue }
        # The game logs at least once per second; silence means a hang or an exception dialog
        $size = (Get-Item $log).Length
        if ($size -ne $lastSize) { $lastSize = $size; $lastGrow = Get-Date }
        elseif (((Get-Date) - $lastGrow).TotalSeconds -gt $Stall) { $reason = "game log silent for $Stall s"; break }
        $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if (-not $text) { continue }
        if ($text -match "\[CRASH\]") { Start-Sleep -Seconds 1; $reason = "crash"; break }
        if ($Until -and $text -match $Until) { $reason = "matched '$Until'"; break }
    }
    if ($Capture -and -not $p.HasExited) {
        Add-Type -AssemblyName System.Drawing
        Add-Type @"
using System; using System.Runtime.InteropServices;
public class AzWin {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  public struct RECT { public int L, T, R, B; }
}
"@
        $p.Refresh()
        $r = New-Object AzWin+RECT
        [AzWin]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [AzWin]::PrintWindow($p.MainWindowHandle, $hdc, 2) | Out-Null
        $g.ReleaseHdc($hdc)
        $bmp.Save("$out\last_shot.png")
        $g.Dispose(); $bmp.Dispose()
    }
} finally {
    Kill-Azahar
    if ($Gdb) { Set-GdbStub $false }
}

$secs = [int]((Get-Date) - $start).TotalSeconds
Write-Output "stopped after $secs s: $reason"
if (Test-Path $log) {
    Copy-Item $log "$out\last_log.txt" -Force
    Get-Content "$out\last_log.txt" | Select-Object -Last 15
} else {
    Write-Output "no log.txt written"
}

# Crash addresses: the game's [CRASH] lines (hardware) or Azahar's exception dump (emulator)
$addrs = @()
$lines = if (Test-Path "$out\last_log.txt") { Get-Content "$out\last_log.txt" } else { @() }
foreach ($l in ($lines | Where-Object { $_ -match "^\[CRASH\]" -and $_ -match "pc=|stack" })) {
    $addrs += [regex]::Matches($l, "(?:pc=|lr=| )([0-9a-f]{8})") | ForEach-Object { $_.Groups[1].Value }
}
$emu = ""
for ($k = 0; $k -lt 10 -and $emu -notmatch "Exception Type:"; $k++) { # the log can stay locked briefly after the kill
    $emu = Read-Tail $emuLog 262144
    if ($emu -notmatch "Exception Type:") { Start-Sleep -Milliseconds 300 }
}
$i = $emu.LastIndexOf("Exception Type:") # Azahar repeats the dump: keep the last one
if ($i -ge 0) {
    $block = $emu.Substring([Math]::Max(0, $emu.LastIndexOf("Thread:", $i)))
    Set-Content "$out\last_exception.txt" $block
    Write-Output "--- Azahar exception (build3ds/last_exception.txt) ---"
    ($block -split "`n") | Where-Object { $_ -match "Exception Type|PC |LR " } | ForEach-Object { $_.Trim() }
    foreach ($m in [regex]::Matches($block, "(?:LR|PC)\s+0x([0-9A-Fa-f]{8})")) { $addrs += $m.Groups[1].Value }
    foreach ($m in [regex]::Matches($block, "(?m)^\s*0x[0-9A-Fa-f]{8}:((?:\s+[0-9A-Fa-f]{2})+)")) {
        $b = ($m.Groups[1].Value.Trim() -split "\s+")
        for ($k = 0; $k + 3 -lt $b.Count; $k += 4) { $addrs += ($b[$k + 3] + $b[$k + 2] + $b[$k + 1] + $b[$k]) }
    }
}
$addrs = $addrs | ForEach-Object { [Convert]::ToUInt32($_, 16) } |
    Where-Object { $_ -ge 0x100000 -and $_ -lt 0x1000000 } | ForEach-Object { "0x{0:x8}" -f $_ } | Select-Object -Unique
if ($addrs) {
    Write-Output "--- crash symbols (pc, lr, then stack words that are code) ---"
    $env:MSYS_NO_PATHCONV = "1"
    & docker run --rm -v ac3ds-work:/work devkitpro/devkitarm:latest sh -c "`$DEVKITARM/bin/arm-none-eabi-addr2line -f -C -p -e /work/build/ac_3ds.elf $($addrs -join ' ')" |
        Where-Object { $_ -notmatch "^\?\?" }
    exit 2
}
exit 0
