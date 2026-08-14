# Simulates window resize / maximize / restore to reproduce UI bugs.
$ErrorActionPreference = 'Stop'

$exe = 'C:\Coding\Work\work4-tools\build\work4-tools.exe'
Get-Process -Name work4-tools -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class WZ {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
}
'@

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 2
$script:hwnd = [IntPtr]::Zero
$cb = [WZ+EnumProc]{
    param($h, $l)
    $pid2 = 0
    [WZ]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $script:proc.Id -and [WZ]::IsWindowVisible($h)) {
        $cs = New-Object System.Text.StringBuilder 128
        [WZ]::GetClassName($h, $cs, 128) | Out-Null
        if ($cs.ToString() -eq 'Work4ToolsMainWnd') { $script:hwnd = $h; return $false }
    }
    return $true
}
[WZ]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'window not found' }
Write-Host "hwnd=$($script:hwnd)"

$crashed = $false
$sizes = @(@(900, 600), @(1280, 800), @(1600, 900), @(1920, 1080), @(2560, 1440),
           @(1100, 700), @(1400, 900), @(800, 600), @(1920, 1000), @(1200, 800))
foreach ($s in $sizes) {
    [void][WZ]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 50, 50, $s[0], $s[1], 0x0040)
    Start-Sleep -Milliseconds 200
    if ($proc.HasExited) {
        Write-Host "CRASHED at $($s[0])x$($s[1]) code=$($proc.ExitCode)"
        $crashed = $true
        break
    }
}
if (-not $crashed) { Write-Host 'survived all fixed sizes' }

# rapid consecutive resizes (like dragging the edge)
for ($i = 0; $i -lt 30 -and -not $crashed; $i++) {
    $w = 700 + ($i % 10) * 130
    $h = 500 + ($i % 8) * 70
    [void][WZ]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 40, 40, $w, $h, 0x0040)
    Start-Sleep -Milliseconds 60
    if ($proc.HasExited) {
        Write-Host "CRASHED during rapid resize at step $i ($w x $h) code=$($proc.ExitCode)"
        $crashed = $true
    }
}
if (-not $crashed) { Write-Host 'survived rapid resize' }

if (-not $crashed) {
    [void][WZ]::ShowWindow($script:hwnd, 3)  # SW_MAXIMIZE
    Start-Sleep -Milliseconds 600
    if ($proc.HasExited) {
        Write-Host "CRASHED on maximize code=$($proc.ExitCode)"
        $crashed = $true
    } else {
        Write-Host 'maximize ok'
        [void][WZ]::ShowWindow($script:hwnd, 9)  # SW_RESTORE
        Start-Sleep -Milliseconds 600
        if ($proc.HasExited) {
            Write-Host "CRASHED on restore code=$($proc.ExitCode)"
            $crashed = $true
        } else {
            Write-Host 'restore ok'
        }
    }
}

if (-not $proc.HasExited) {
    [void][WZ]::ShowWindow($script:hwnd, 3)
    Start-Sleep -Milliseconds 400
    $h = Get-Process -Id $proc.Id
    Write-Host "final state: alive, WS=$([math]::Round($h.WorkingSet64/1MB,1))MB"
    Stop-Process -Id $proc.Id -Force
}
Write-Host "RESULT: $(if ($crashed) { 'CRASHED' } else { 'PASSED' })"
