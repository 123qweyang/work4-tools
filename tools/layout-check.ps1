# Dumps child control rects (client coords) for the work4-tools main window.
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class LC {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@

$proc = Get-Process -Name work4-tools -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw 'work4-tools not running' }
$script:hwnd = [IntPtr]::Zero
$cb = [LC+EnumProc]{
    param($h, $l)
    $pid2 = 0
    [LC]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $script:proc.Id -and [LC]::IsWindowVisible($h)) {
        $cs = New-Object System.Text.StringBuilder 128
        [LC]::GetClassName($h, $cs, 128) | Out-Null
        if ($cs.ToString() -eq 'Work4ToolsMainWnd') { $script:hwnd = $h; return $false }
    }
    return $true
}
[LC]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'window not found' }

$script:rows = [System.Collections.Generic.List[string]]::new()
$ccb = [LC+EnumProc]{
    param($h, $l)
    $cs = New-Object System.Text.StringBuilder 128
    [LC]::GetClassName($h, $cs, 128) | Out-Null
    $cls = $cs.ToString()
    $txt = New-Object System.Text.StringBuilder 64
    [LC]::GetWindowText($h, $txt, 64) | Out-Null
    $r = New-Object LC+RECT
    [LC]::GetWindowRect($h, [ref]$r) | Out-Null
    $p1 = New-Object LC+POINT; $p1.X = $r.L; $p1.Y = $r.T
    $p2 = New-Object LC+POINT; $p2.X = $r.R; $p2.Y = $r.B
    [void][LC]::ScreenToClient($script:hwnd, [ref]$p1)
    [void][LC]::ScreenToClient($script:hwnd, [ref]$p2)
    $id = [LC]::GetDlgCtrlID($h)
    $script:rows.Add(("{0,-22} id={1,-5} text='{2}' rect=({3},{4})-({5},{6})" -f $cls, $id, $txt.ToString(), $p1.X, $p1.Y, $p2.X, $p2.Y))
    return $true
}
[LC]::EnumChildWindows($script:hwnd, $ccb, [IntPtr]::Zero) | Out-Null
$mr = New-Object LC+RECT
[LC]::GetWindowRect($script:hwnd, [ref]$mr) | Out-Null
Write-Host "Main window: ($($mr.L),$($mr.T))-($($mr.R),$($mr.B)) size $($mr.R-$mr.L)x$($mr.B-$mr.T)"
$script:rows | ForEach-Object { Write-Host $_ }
