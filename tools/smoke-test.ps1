# Work4 文件快搜 - 自动化冒烟测试
# 验证：窗口创建、目录浏览、全盘索引构建、关键词搜索、内存占用
$ErrorActionPreference = 'Stop'
$exe = 'C:\Coding\Work\work4-tools\build\work4-tools.exe'
$script:t0 = Get-Date

# 清理残留测试实例（单实例程序）
Get-Process -Name work4-tools -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W32 {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessageStr(IntPtr h, uint msg, IntPtr w, string l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessagePtr(IntPtr h, uint msg, IntPtr w, IntPtr l);

    public static string GetStatusText(IntPtr status, int part) {
        IntPtr buf = Marshal.AllocHGlobal(2048);
        try {
            SendMessagePtr(status, 0x0402, (IntPtr)part, buf);
            return Marshal.PtrToStringUni(buf);
        } finally {
            Marshal.FreeHGlobal(buf);
        }
    }
}
'@

$proc = Start-Process -FilePath $exe -PassThru
Write-Host "Started PID $($proc.Id)"

# 1) 等待主窗口（按 PID 枚举顶层窗口）
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 200 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 100
    $cb = [W32+EnumProc]{
        param($h, $lp)
        $pid2 = 0
        [W32]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
        if ($pid2 -eq $script:proc.Id -and [W32]::IsWindowVisible($h)) {
            $cs = New-Object System.Text.StringBuilder 128
            [W32]::GetClassName($h, $cs, 128) | Out-Null
            if ($cs.ToString() -eq 'Work4ToolsMainWnd') {
                $script:hwnd = $h
                return $false
            }
        }
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($proc.HasExited) { throw "Process exited early, code $($proc.ExitCode)" }
}
if ($hwnd -eq [IntPtr]::Zero) { throw 'Main window not found' }
$title = New-Object System.Text.StringBuilder 256
[W32]::GetWindowText($hwnd, $title, 256) | Out-Null
Write-Host "PASS: window created, title='$($title.ToString())'"

# 记录 CPU 时间用于多线程占用验证
$cpu0 = (Get-Process -Id $proc.Id).CPU

# 2) 枚举子窗口
$edits = [System.Collections.Generic.List[IntPtr]]::new()
$classes = [System.Collections.Generic.List[string]]::new()
$listView = [IntPtr]::Zero
$status = [IntPtr]::Zero
$cb = [W32+EnumProc]{
    param($h, $lp)
    $sb = New-Object System.Text.StringBuilder 128
    [W32]::GetClassName($h, $sb, 128) | Out-Null
    $cls = $sb.ToString()
    $script:classes.Add($cls)
    if ($cls -eq 'Edit') { $script:edits.Add($h) }
    if ($cls -eq 'SysListView32') { $script:listView = $h }
    if ($cls -eq 'msctls_statusbar32') { $script:status = $h }
    return $true
}
[W32]::EnumChildWindows($hwnd, $cb, [IntPtr]::Zero) | Out-Null
$classSummary = ($classes | Group-Object | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ', '
Write-Host "Child controls: $classSummary"
if ($edits.Count -lt 2) { throw 'Search edit not found' }
if ($listView -eq [IntPtr]::Zero) { throw 'ListView not found' }
if ($status -eq [IntPtr]::Zero) { throw 'StatusBar not found' }
$searchEdit = $edits[$edits.Count - 1]

# 3) 浏览验证：初始目录列表应非空
$LVM_GETITEMCOUNT = 0x1004
$SB_GETTEXT = 0x0402
$initial = [W32]::SendMessage($listView, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
Write-Host "Initial ListView items: $initial"
if ($initial -le 0) { throw 'Initial directory listing is empty' }
Write-Host 'PASS: directory browse works'

$mem1 = (Get-Process -Id $proc.Id).WorkingSet64 / 1MB
Write-Host "Memory before index ready: $([math]::Round($mem1,1)) MB"
Write-Host "CPU after browse: $([math]::Round((Get-Process -Id $proc.Id).CPU,1)) s"

# 4) 等待全盘索引构建完成（索引文件出现）
Write-Host 'Waiting for full-disk index to be built...'
$idxFile = 'C:\Coding\Work\work4-tools\build\index\file-index.bin'
$indexReady = $false
for ($i = 0; $i -lt 1200; $i++) {
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) { throw "Process exited during index build, code $($proc.ExitCode)" }
    if (Test-Path $idxFile) { $indexReady = $true; break }
}
if (-not $indexReady) { throw 'Index not ready within 10 minutes' }
Write-Host "PASS: index built"

if (-not (Test-Path $idxFile)) { throw 'index file not written' }
$idxSize = (Get-Item $idxFile).Length
Write-Host "PASS: index file written, size=$idxSize bytes"
$cpu1 = (Get-Process -Id $proc.Id).CPU
$wall = (Get-Date) - $script:t0
Write-Host "Index build wall time: $([math]::Round($wall.TotalSeconds,1)) s, CPU time: $([math]::Round($cpu1 - $cpu0,1)) s"

# 5) 搜索验证
[W32]::SendMessageStr($searchEdit, 0x000C, [IntPtr]::Zero, 'windows') | Out-Null
Write-Host 'Searching "windows"...'
$found = $false
$prev = -1
$stable = 0
for ($i = 0; $i -lt 240; $i++) {
    Start-Sleep -Milliseconds 250
    if ($proc.HasExited) { throw 'Process exited during search' }
    $cnt = [W32]::SendMessage($listView, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
    if ($cnt -eq $prev) { $stable++ } else { $stable = 0 }
    $prev = $cnt
    # 列表从"浏览项数"清空后重新填充，连续 1 秒数值稳定视为搜索完成
    if ($cnt -gt 0 -and $cnt -ne $initial -and $stable -ge 4) { $found = $true; break }
}
if (-not $found) { throw 'Search returned no results' }
Write-Host "PASS: search results=$cnt"

$mem2 = (Get-Process -Id $proc.Id).WorkingSet64 / 1MB
Write-Host "Memory after index+search: $([math]::Round($mem2,1)) MB"

Stop-Process -Id $proc.Id -Force
Write-Host 'SMOKE TEST PASSED'
