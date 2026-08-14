# Verifies dark mode toggle + search highlight via DWM attribute, control text,
# window screenshots and pixel analysis.
$ErrorActionPreference = 'Continue'

$exe = 'C:\Coding\Work\work4-tools\build\work4-tools.exe'
Get-Process -Name work4-tools -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-ItemProperty -Path 'HKCU:\Software\Work4Tools' -Name DarkMode -ErrorAction SilentlyContinue

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class VD {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int sz);
}
'@

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$script:hwnd = [IntPtr]::Zero
$cb = [VD+EnumProc]{
    param($h, $l)
    $pid2 = 0
    [VD]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $script:proc.Id -and [VD]::IsWindowVisible($h)) {
        $cs = New-Object System.Text.StringBuilder 128
        [VD]::GetClassName($h, $cs, 128) | Out-Null
        if ($cs.ToString() -eq 'Work4ToolsMainWnd') { $script:hwnd = $h; return $false }
    }
    return $true
}
[VD]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'window not found' }

$script:darkBtn = [IntPtr]::Zero
$script:searchEdit = [IntPtr]::Zero
$ccb = [VD+EnumProc]{
    param($h, $l)
    $sb = New-Object System.Text.StringBuilder 128
    [VD]::GetClassName($h, $sb, 128) | Out-Null
    $id = [VD]::GetDlgCtrlID($h)
    if ($id -eq 1009) { $script:darkBtn = $h }
    if ($sb.ToString() -eq 'Edit') { $script:searchEdit = $h }
    return $true
}
[VD]::EnumChildWindows($script:hwnd, $ccb, [IntPtr]::Zero) | Out-Null

# --- 1) light mode screenshot ---
& 'C:\Coding\Work\work4-tools\tools\ui-shot.ps1' -OutPath 'C:\Coding\Work\work4-tools\tools\v-light.png' | Out-Null
Write-Host 'light screenshot saved'

# --- 2) toggle dark mode ---
[void][VD]::SendMessage($script:hwnd, 0x0111, [IntPtr]1009, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800
$txt = New-Object System.Text.StringBuilder 64
[VD]::GetWindowText($script:darkBtn, $txt, 64) | Out-Null
$dwm = 0
[void][VD]::DwmGetWindowAttribute($script:hwnd, 20, [ref]$dwm, 4)
Write-Host "dark button text = '$($txt.ToString())', DWM dark = $dwm"
& 'C:\Coding\Work\work4-tools\tools\ui-shot.ps1' -OutPath 'C:\Coding\Work\work4-tools\tools\v-dark.png' | Out-Null
Write-Host 'dark screenshot saved'

# --- 3) search with highlight ---
[void][VD]::SendMessageStr($script:searchEdit, 0x000C, [IntPtr]::Zero, 'windows')
Start-Sleep -Seconds 3
& 'C:\Coding\Work\work4-tools\tools\ui-shot.ps1' -OutPath 'C:\Coding\Work\work4-tools\tools\v-search.png' | Out-Null
Write-Host 'search screenshot saved'

# --- 4) restore light ---
[void][VD]::SendMessage($script:hwnd, 0x0111, [IntPtr]1009, [IntPtr]::Zero)
Start-Sleep -Milliseconds 500
$reg = Get-ItemProperty -Path 'HKCU:\Software\Work4Tools' -Name DarkMode -ErrorAction SilentlyContinue
Write-Host "saved dark pref = $($reg.DarkMode)"
Stop-Process -Id $proc.Id -Force
