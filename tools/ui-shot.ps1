# Captures the work4-tools window via PrintWindow at current size.
param([string]$OutPath = 'C:\Coding\Work\work4-tools\tools\ui.png')
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class US {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@

Add-Type -AssemblyName System.Drawing

$proc = Get-Process -Name work4-tools -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw 'work4-tools not running' }
$script:hwnd = [IntPtr]::Zero
$cb = [US+EnumProc]{
    param($h, $l)
    $pid2 = 0
    [US]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $script:proc.Id -and [US]::IsWindowVisible($h)) {
        $cs = New-Object System.Text.StringBuilder 128
        [US]::GetClassName($h, $cs, 128) | Out-Null
        if ($cs.ToString() -eq 'Work4ToolsMainWnd') { $script:hwnd = $h; return $false }
    }
    return $true
}
[US]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'window not found' }

$r = New-Object US+RECT
[US]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L
$h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][US]::PrintWindow($script:hwnd, $hdc, 0)
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $OutPath ($w x $h)"
