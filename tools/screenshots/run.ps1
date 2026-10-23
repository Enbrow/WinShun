# Runs a scene file with the elevated agent, the way the live tests are meant to be (F:\wswt\RULES.md "Live testing"):
#   tools\screenshots\run.ps1 [-Scene scenes\release013.txt] [-Exe <WinShun.exe under test>]
# waits for 20 s without the user's input, takes F:\wswt\ui.lock (waits while another session has it), runs agent.ps1
# elevated (no prompt on this machine) and waits for it, then puts everything back whatever happened: the theme and
# wallpaper (shots.py restore), the cover, the user's own WinShun (dist, not elevated), the lock. Run it from a normal
# (not elevated) PowerShell: that is how the user's WinShun is started again.
param([string]$Scene = 'scenes\release013.txt', [string]$Exe)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class IdleMs {
    [StructLayout(LayoutKind.Sequential)] struct LII { public uint size, time; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LII i);
    public static uint Get() { var i = new LII { size = 8 }; GetLastInputInfo(ref i); return (uint)Environment.TickCount - i.time; }
}
'@
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$userExe = Join-Path $root 'dist\WinShun\WinShun.exe'
$lock = 'F:\wswt\ui.lock'
$sw = [Diagnostics.Stopwatch]::StartNew()
while ([IdleMs]::Get() -lt 20000) {
    if ($sw.Elapsed.TotalMinutes -gt 10) { throw 'the user is at the computer: not starting' }
    Start-Sleep -Seconds 2
}
$sw.Restart()
while ($true) {
    try { $fs = [IO.File]::Open($lock, 'CreateNew', 'Write'); $fs.Close(); break } catch { }
    if ($sw.Elapsed.TotalMinutes -gt 15) { throw "$lock is held by another session: not starting" }
    Start-Sleep -Seconds 3
}
try {
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $here 'agent.ps1'), '-Scene', $Scene)
    if ($Exe) { $args2 += @('-Exe', $Exe) }
    $p = Start-Process powershell.exe -Verb RunAs -PassThru -WindowStyle Hidden -ArgumentList $args2
    $p.WaitForExit()
    Write-Host "agent exit code $($p.ExitCode); log: $(Join-Path $here 'work\agent.txt')"
} finally {
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) { & $py (Join-Path $here 'shots.py') restore }
    # the cover window, if the agent died with it up (its stop file ends it)
    New-Item -ItemType File (Join-Path $here 'work\cover.stop') -Force | Out-Null; Start-Sleep -Seconds 2
    Remove-Item (Join-Path $here 'work\cover.stop') -ErrorAction SilentlyContinue
    # the user's own copy, from this (not elevated) shell, never from a .bak
    if ($Exe) { Start-Process $Exe -ArgumentList '--quit' -WindowStyle Normal; Start-Sleep -Seconds 4 }
    if (-not (Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $userExe })) {
        Start-Process $userExe -ArgumentList '--background' -WindowStyle Normal
        Start-Sleep -Seconds 5
    }
    Write-Host ("the user's WinShun running: " + [bool](Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $userExe }))
    if (Test-Path $lock) { [IO.File]::Delete($lock) }
}
