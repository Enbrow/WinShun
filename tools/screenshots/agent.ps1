# Elevated input agent for the shots: sets the windows up by real input and
# has shots.py cut them out. Run it from an elevated PowerShell (WinShun runs
# elevated; input into it and the backdrop behind it need the same):
#   agent.ps1 scenes\all.txt   runs that file, then exits
#   agent.ps1                  runs each batch written to work\cmd.txt, until 15 idle minutes
# It logs to work\agent.txt. Every key and click first checks that WinShun (or
# a program `allow`ed) is in front, and a click also that the point is on that
# window; a batch stops at the first check that fails. Never Ctrl+1/2/3
# (PixPin's hotkeys on this machine). Keys are virtual-key codes in hex.
#   # comment | run <file> | let <name> <value> ({name} in later lines becomes the value) | sleep <ms> | exit
#   open <query> | settings | allow <process name>
#   tap <vk> | down <vk> | up <vk> | chord <mod vk> <vk> | type <text> (ASCII; other text: clip + Ctrl+V)
#   clickfg <dx> <dy> (physical px from the front window's frame) | clickb <dx> <up from bottom>
#   clickws <x> <y> (on a window of WinShun's) | clickbox <x> <y> (on its taskbar box) | wheel <dx> <dy> <delta>
#   shot <name> (the front window) | shotabs <name> <x> <y> <w> <h>        -> work\shots\<name>.png
#   matte <name> [winshun] (cut out by shots.py shoot)                      -> work\shots\<name>.png
#   scene [--theme light|dark] [--wallpaper NAME] | restore
#   profile start | profile stop (WinShun on work\profile, made if missing; then the user's own again)
#   notepad <file> | paint <file> (files of this folder) | explorer open <folder of the demo> | explorer close
#   canceldialogs | clip <text> | clipfile <path>
param([string]$Scene)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class A {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int size);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint data; public uint dwFlags; public uint time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void Key(ushort vk, bool up) {
        var i = new INPUT[1]; i[0].type = 1; i[0].ki.wVk = vk;
        bool ext = vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28 || vk == 0x2E || vk == 0x24 || vk == 0x23 || vk == 0x21 || vk == 0x22;
        i[0].ki.dwFlags = (up ? 2u : 0u) | (ext ? 1u : 0u);
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Type(string text) {
        foreach (char c in text) {
            var i = new INPUT[2];
            i[0].type = 1; i[0].ki.wScan = c; i[0].ki.dwFlags = 4;
            i[1].type = 1; i[1].ki.wScan = c; i[1].ki.dwFlags = 6;
            SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
        }
    }
    public static void Wheel(int delta) {
        var i = new INPUT[1]; i[0].type = 0; i[0].mi.dwFlags = 0x800; i[0].mi.data = (uint)delta;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Click() {
        var i = new INPUT[2]; i[0].type = 0; i[0].mi.dwFlags = 2; i[1].type = 0; i[1].mi.dwFlags = 4;
        SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
    public static RECT Frame(IntPtr h) { RECT r; DwmGetWindowAttribute(h, 9, out r, 16); return r; }
    public static IntPtr RootAt(int x, int y) { var p = new POINT(); p.X = x; p.Y = y; return GetAncestor(WindowFromPoint(p), 2); }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    public static string ClassAt(int x, int y) { var p = new POINT(); p.X = x; p.Y = y; var s = new System.Text.StringBuilder(64); GetClassNameW(WindowFromPoint(p), s, 64); return s.ToString(); }
}
'@
[A]::SetProcessDPIAware() | Out-Null
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$work = Join-Path $here 'work'
$shots = Join-Path $work 'shots'
$profileDir = Join-Path $work 'profile'
New-Item -ItemType Directory -Force $shots | Out-Null
$cmdFile = Join-Path $work 'cmd.txt'
$log = Join-Path $work 'agent.txt'
$exe = Join-Path $root 'dist\WinShun\WinShun.exe'
$tool = Join-Path $here 'shots.py'
$demo = 'F:\Win' + [char]0x987A + [char]0x6F14 + [char]0x793A # the demo folder the shots show (stage.py's DEMO)
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { $python = (Get-Command py -ErrorAction Stop).Source }
[Console]::OutputEncoding = [Text.Encoding]::UTF8; $env:PYTHONIOENCODING = 'utf-8'

function Note($s) { # never throws: a reader may hold the log for a moment
    $line = "{0:HH:mm:ss.fff} {1}`r`n" -f (Get-Date), $s
    for ($i = 0; $i -lt 40; $i++) { try { [IO.File]::AppendAllText($log, $line, [Text.Encoding]::UTF8); return } catch { Start-Sleep -Milliseconds 50 } }
}
$allowed = @()
function WsPid { $p = Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1; if ($p) { [uint32]$p.Id } else { 0 } }
function InFront {
    $pid0 = [A]::Pid([A]::GetForegroundWindow())
    if ($pid0 -eq (WsPid)) { return $true }
    try { $name = (Get-Process -Id $pid0).ProcessName } catch { return $false }
    return $allowed -contains $name
}
function Guard { if (-not (InFront)) { throw "not in front (pid $([A]::Pid([A]::GetForegroundWindow())))" } }
function Vk($s) { return [uint16]([Convert]::ToInt32($s, 16)) }
function WaitFront { $sw = [Diagnostics.Stopwatch]::StartNew(); while ($sw.ElapsedMilliseconds -lt 5000 -and -not (InFront)) { Start-Sleep -Milliseconds 20 }; Guard }
function Local($p) { # a path in this folder (relative ones from here), or an error
    $full = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $here $p }))
    if (-not $full.StartsWith($here + '\')) { throw "only files of $here here: $p" }
    return $full
}
function Shots { # shots.py with these arguments, its output in the log
    $out = & $python $tool @args 2>&1 | Out-String
    Note $out.Trim()
    if ($LASTEXITCODE -ne 0) { throw "shots.py $($args[0]) failed ($LASTEXITCODE)" }
}
function QuitWs {
    if (-not (WsPid)) { return }
    Start-Process -FilePath $exe -ArgumentList '--quit' -WindowStyle Normal
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ((WsPid) -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 200 }
    if (WsPid) { throw 'WinShun did not quit' }
}
function StartWs($more) {
    Start-Process -FilePath $exe -ArgumentList (@('--background') + $more) -WindowStyle Normal
    Start-Sleep -Seconds 6
    if (-not (WsPid)) { throw 'WinShun did not stay up' }
    Note "WinShun up (pid $(WsPid)) $more"
}
function Save($bmp, $name) { $bmp.Save((Join-Path $shots "$name.png"), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }

$vars = @{}
function RunLines($lines) {
    foreach ($line in $lines) {
        foreach ($k in $vars.Keys) { $line = $line.Replace("{$k}", $vars[$k]) }
        $a = $line.Trim() -split '\s+', 2
        if (-not $a[0] -or $a[0].StartsWith('#')) { continue }
        $arg = if ($a.Count -gt 1) { $a[1] } else { '' }
        switch ($a[0]) {
            'run' { RunLines (Get-Content (Local $arg) -Encoding UTF8) }
            'let' { $p = $arg -split '\s+', 2; $vars[$p[0]] = $p[1] }
            'open' { Start-Process -FilePath $exe -ArgumentList '--query', $arg -WindowStyle Normal; WaitFront; Start-Sleep -Milliseconds 600 }
            'settings' { Start-Process -FilePath $exe -ArgumentList '--settings' -WindowStyle Normal; WaitFront; Start-Sleep -Milliseconds 800 }
            'allow' { $script:allowed += $arg }
            'tap' { Guard; $k = Vk $arg; [A]::Key($k, $false); [A]::Key($k, $true) }
            'down' { Guard; [A]::Key((Vk $arg), $false) }
            'up' { [A]::Key((Vk $arg), $true) }
            'chord' {
                $p = $arg -split '\s+'; $m = Vk $p[0]; $k = Vk $p[1]
                if ($m -eq 0x11 -and $k -ge 0x31 -and $k -le 0x33) { throw 'never Ctrl+1/2/3 here' }
                Guard; [A]::Key($m, $false); [A]::Key($k, $false); [A]::Key($k, $true); [A]::Key($m, $true)
            }
            { $_ -eq 'clickfg' -or $_ -eq 'clickb' } {
                Guard
                $p = $arg -split '\s+'; $h = [A]::GetForegroundWindow(); $r = [A]::Frame($h)
                $x = $r.L + [int]$p[0]
                $y = if ($a[0] -eq 'clickb') { $r.B - [int]$p[1] } else { $r.T + [int]$p[1] }
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::RootAt($x, $y) -ne $h) { throw "click at $x,$y would land on another window" }
                Guard; [A]::Click()
            }
            'clickws' { # absolute physical px, on a window of WinShun's own (the bar by a dialog)
                $p = $arg -split '\s+'; $x = [int]$p[0]; $y = [int]$p[1]
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::Pid([A]::RootAt($x, $y)) -ne (WsPid)) { throw "click at $x,$y would land on another program" }
                [A]::Click()
            }
            'clickbox' { # absolute physical px, on WinShun's search box in the taskbar
                $p = $arg -split '\s+'; $x = [int]$p[0]; $y = [int]$p[1]
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::ClassAt($x, $y) -ne 'WinShun.TaskbarSearchBox') { throw "click at $x,$y is not on the taskbar box" }
                [A]::Click()
            }
            'wheel' {
                Guard
                $p = $arg -split '\s+'; $h = [A]::GetForegroundWindow(); $r = [A]::Frame($h)
                $x = $r.L + [int]$p[0]; $y = $r.T + [int]$p[1]
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::RootAt($x, $y) -ne $h) { throw "wheel at $x,$y would land on another window" }
                [A]::Wheel([int]$p[2])
            }
            'type' { Guard; [A]::Type($arg) }
            'shot' {
                $r = [A]::Frame([A]::GetForegroundWindow())
                $bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
                $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
                Save $bmp $arg
            }
            'shotabs' { # name x y w h
                $p = $arg -split '\s+'
                $bmp = New-Object Drawing.Bitmap ([int]$p[3]), ([int]$p[4])
                $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen([int]$p[1], [int]$p[2], 0, 0, $bmp.Size); $g.Dispose()
                Save $bmp $p[0]
            }
            'matte' { # <name> [winshun]
                $m = $arg -split '\s+'
                if ($m.Count -gt 1 -and $m[1] -eq 'winshun') { Shots shoot -o (Join-Path $shots "$($m[0]).png") --winshun }
                else { Guard; Shots shoot -o (Join-Path $shots "$($m[0]).png") }
            }
            'scene' { Shots scene @($arg -split '\s+') }
            'restore' { Shots restore }
            'profile' { # start: WinShun on work\profile, with a fresh clipboard history; stop: the user's own again
                QuitWs
                if ($arg -eq 'start') {
                    if (-not (Test-Path (Join-Path $profileDir 'made-for-screenshots.txt'))) { Shots profile $profileDir }
                    $clips = Join-Path $profileDir 'clipboard'
                    if (Test-Path $clips) { Remove-Item $clips -Recurse -Force }
                    StartWs @('--profile', "`"$profileDir`"")
                } elseif ($arg -eq 'stop') { StartWs @() } else { throw "profile what? '$arg'" }
            }
            'notepad' { # Notepad on a file of this folder (made if missing), which then counts as "in front"
                $f = Local $arg
                if (-not (Test-Path $f)) { New-Item -ItemType File $f | Out-Null }
                Start-Process -FilePath 'notepad.exe' -ArgumentList "`"$f`""
                $script:allowed += 'Notepad'; WaitFront; Start-Sleep -Milliseconds 1200
            }
            'paint' { # Paint on a picture of this folder, which then counts as "in front"
                Start-Process -FilePath 'mspaint.exe' -ArgumentList "`"$(Local $arg)`""
                $script:allowed += 'mspaint'; WaitFront; Start-Sleep -Milliseconds 2500
            }
            'explorer' { # open <folder of the demo> | close: the Explorer windows on the demo folder
                $p = $arg -split '\s+', 2
                if ($p[0] -eq 'open') {
                    if (-not $p[1].StartsWith($demo)) { throw 'explorer only on the demo folder' }
                    Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$($p[1])`""; Start-Sleep -Milliseconds 2000
                } else {
                    foreach ($w in @((New-Object -ComObject Shell.Application).Windows())) {
                        try { if ($w.Document.Folder.Self.Path.StartsWith($demo)) { $w.Quit() } } catch {}
                    }
                }
            }
            'canceldialogs' { # Notepad's open dialogs (from Ctrl+O) get Cancel
                $h = [IntPtr]::Zero; $n = 0
                while (($h = [A]::FindWindowExW([IntPtr]::Zero, $h, '#32770', [NullString]::Value)) -ne [IntPtr]::Zero) {
                    if (-not [A]::IsWindowVisible($h)) { continue }
                    try { $name = (Get-Process -Id ([int][A]::Pid($h))).ProcessName } catch { continue }
                    if ($name -eq 'Notepad') { [A]::PostMessageW($h, 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null; $n++ }
                }
                Note "cancelled $n dialogs"; Start-Sleep -Milliseconds 500
            }
            'clip' { Set-Clipboard -Value $arg; Start-Sleep -Milliseconds 400 }
            'clipfile' { Set-Clipboard -Path $arg; Start-Sleep -Milliseconds 400 }
            'sleep' { Start-Sleep -Milliseconds ([int]$arg) }
            'exit' { Note 'exit'; exit }
            default { throw "unknown: $line" }
        }
        Note "ok: $line"
    }
}

function Batch($lines) {
    try { RunLines $lines }
    catch {
        foreach ($k in 0x10, 0x11, 0x12, 0x5B) { [A]::Key([uint16]$k, $true) } # nothing left held
        Note "BLOCKED: $_"
        return $false
    }
    Note 'batch done'
    return $true
}

if (-not (Test-Path $exe)) { throw "no $exe; build and deploy first" }
Note "agent up, WinShun pid $(WsPid)"
if ($Scene) {
    if (-not (Batch @("run $Scene"))) { Write-Host "stopped, see $log"; exit 1 }
    exit
}
$idle = [Diagnostics.Stopwatch]::StartNew()
while ($idle.Elapsed.TotalMinutes -lt 15) {
    if (-not (Test-Path $cmdFile)) { Start-Sleep -Milliseconds 200; continue }
    $lines = Get-Content $cmdFile -Encoding UTF8
    [IO.File]::Delete($cmdFile)
    $idle.Restart()
    Batch $lines | Out-Null
}
Note 'idle: exit'
