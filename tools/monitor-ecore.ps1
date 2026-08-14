# Monitors P-core vs E-core usage while work4-tools rebuilds its index.
# For decisive proof, it samples per-thread affinity masks + CPU times twice
# inside the rebuild window, so E-core-bound scan workers show CPU time growth
# while unbound threads stay near zero.
$ErrorActionPreference = 'Continue'

$exe = 'C:\Coding\Work\work4-tools\build\work4-tools.exe'
$idxFile = 'C:\Coding\Work\work4-tools\build\index\file-index.bin'
$logFile = 'C:\Coding\Work\work4-tools\build\index\build.log'

Get-Process -Name work4-tools -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -LiteralPath $idxFile -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $logFile -ErrorAction SilentlyContinue

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public struct THREADENTRY32 {
    public uint dwSize; public uint cntUsage; public uint th32ThreadID;
    public uint th32OwnerProcessID; public int tpBasePri; public int tpDeltaPri; public uint dwFlags;
}
[StructLayout(LayoutKind.Sequential)]
public struct GROUP_AFFINITY {
    public IntPtr Mask;
    public ushort Group;
    public ushort Reserved1;
    public ushort Reserved2;
    public ushort Reserved3;
}
public static class TUtil {
    [DllImport("kernel32.dll")] public static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll")] public static extern bool Thread32First(IntPtr snap, ref THREADENTRY32 te);
    [DllImport("kernel32.dll")] public static extern bool Thread32Next(IntPtr snap, ref THREADENTRY32 te);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] public static extern bool GetThreadGroupAffinity(IntPtr h, out GROUP_AFFINITY ga);
    [DllImport("kernel32.dll")] public static extern bool GetThreadTimes(IntPtr h, out long c, out long e, out long k, out long u);
    [DllImport("ntdll.dll")] public static extern int NtQueryInformationThread(IntPtr h, int cls, out IntPtr info, int len, out int ret);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@

function Get-ThreadSnapshot([uint32]$pidNum) {
    $result = [System.Collections.Generic.List[object]]::new()
    $snap = [IntPtr]::Zero
    try { $snap = [TUtil]::CreateToolhelp32Snapshot(0x4, 0) } catch { }
    if ($snap -eq [IntPtr]::Zero) { return $result }
    $te = New-Object THREADENTRY32
    $te.dwSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][THREADENTRY32])
    if ([TUtil]::Thread32First($snap, [ref]$te)) {
        do {
            if ($te.th32OwnerProcessID -eq $pidNum) {
                $h = [TUtil]::OpenThread(0x800, $false, $te.th32ThreadID)
                if ($h -ne [IntPtr]::Zero) {
                    $k = 0L; $u = 0L; $c = 0L; $e = 0L
                    $mask = -1L
                    $info = [IntPtr]::Zero
                    $ret = 0
                    # ThreadAffinityMask info class = 3（内核线程真实亲和掩码）
                    if ([TUtil]::NtQueryInformationThread($h, 3, [ref]$info, [System.Runtime.InteropServices.Marshal]::SizeOf([type][IntPtr]), [ref]$ret) -eq 0) {
                        $mask = $info.ToInt64()
                    } else {
                        $ga = New-Object GROUP_AFFINITY
                        if ([TUtil]::GetThreadGroupAffinity($h, [ref]$ga)) {
                            $mask = $ga.Mask.ToInt64()
                        }
                    }
                    if ([TUtil]::GetThreadTimes($h, [ref]$c, [ref]$e, [ref]$k, [ref]$u)) {
                        $result.Add([pscustomobject]@{
                            tid = $te.th32ThreadID
                            mask = $mask
                            cpuSec = [math]::Round((($k + $u) / 10000000.0), 4)
                        })
                    }
                    [TUtil]::CloseHandle($h) | Out-Null
                }
            }
        } while ([TUtil]::Thread32Next($snap, [ref]$te))
    }
    [TUtil]::CloseHandle($snap) | Out-Null
    return $result
}

$proc = Start-Process -FilePath $exe -PassThru
$pidNum = $proc.Id
Write-Host "Started PID $pidNum"

# pin this monitor to CPU 0 (P-core) so sampling overhead does not pollute E-core readings
try { (Get-Process -Id $PID).ProcessorAffinity = 1 } catch { }

try { Get-Counter '\Processor(*)\% Processor Time' -SampleInterval 1 -MaxSamples 1 | Out-Null } catch { }

$samples = [System.Collections.Generic.List[object]]::new()
$snapA = $null
$snapB = $null
$snapBTime = $null
$logState = 'waiting'  # waiting -> started -> done
$start = Get-Date
while (((Get-Date) - $start).TotalSeconds -lt 10) {
    $elapsed = ((Get-Date) - $start).TotalSeconds
    try {
        $c = Get-Counter '\Processor(*)\% Processor Time' -ErrorAction SilentlyContinue
        if ($c) {
            foreach ($s in $c.CounterSamples) {
                if ($s.InstanceName -match '^\d+$') {
                    $samples.Add([pscustomobject]@{
                        t = [math]::Round($elapsed, 2)
                        cpu = [int]$s.InstanceName
                        v = [math]::Round([double]$s.CookedValue, 1)
                    })
                }
            }
        }
    } catch { }

    if ($logState -eq 'waiting' -and (Test-Path $logFile)) {
        $log = Get-Content $logFile -Encoding Unicode -ErrorAction SilentlyContinue
        if ($log -match 'rebuild start') {
            $logState = 'started'
            Start-Sleep -Milliseconds 300
            $snapA = Get-ThreadSnapshot $pidNum
            Write-Host ("Snapshot A after rebuild start ({0:N1}s): {1} threads" -f ((Get-Date) - $start).TotalSeconds, $snapA.Count)
            $snapBTime = (Get-Date).AddSeconds(1.0)
        }
    }
    if ($logState -eq 'started' -and $null -eq $snapB -and $snapBTime -and (Get-Date) -ge $snapBTime) {
        $snapB = Get-ThreadSnapshot $pidNum
        Write-Host ("Snapshot B at {0:N1}s: {1} threads" -f ((Get-Date) - $start).TotalSeconds, $snapB.Count)
        $logState = 'done'
    }
    Start-Sleep -Milliseconds 100
}

$done = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-Path $logFile) {
        $log = Get-Content $logFile -Encoding Unicode -ErrorAction SilentlyContinue
        if ($log -match 'index saved') { $done = $true; break }
    }
    if ($proc.HasExited) { break }
}
Write-Host "Rebuild done: $done"

# per-thread affinity + CPU delta (workers may exit before snapshot B)
Write-Host "Per-thread affinity / CPU (kind | mask | tid | cpuAtA | deltaAB):"
if ($snapA -and $snapB) {
    foreach ($a in $snapA) {
        $b = $snapB | Where-Object { $_.tid -eq $a.tid } | Select-Object -First 1
        $delta = if ($b) { $b.cpuSec - $a.cpuSec } else { -1 }
        $kind = if ($a.mask -eq 0xFF0000) { 'E-CORE-BOUND' } else { 'unbound' }
        Write-Host ("  {0,-13} mask=0x{1:X} tid={2,-6} cpuA={3:N3}s delta={4:N3}s" -f $kind, $a.mask, $a.tid, $a.cpuSec, $delta)
    }
    Write-Host ("  E-core-bound worker threads in snapshot A: {0}" -f (($snapA | Where-Object { $_.mask -eq 0xFF0000 }).Count))
} else {
    Write-Host "  (snapshots incomplete)"
}

$pSamples = $samples | Where-Object { $_.cpu -lt 16 }
$eSamples = $samples | Where-Object { $_.cpu -ge 16 }
$pAvg = ($pSamples | Measure-Object -Property v -Average).Average
$eAvg = ($eSamples | Measure-Object -Property v -Average).Average
$pMax = ($pSamples | Measure-Object -Property v -Maximum).Maximum
$eMax = ($eSamples | Measure-Object -Property v -Maximum).Maximum
Write-Host ("P-core (0-15) avg={0:N1}% max={1:N1}% samples={2}" -f $pAvg, $pMax, $pSamples.Count)
Write-Host ("E-core (16-23) avg={0:N1}% max={1:N1}% samples={2}" -f $eAvg, $eMax, $eSamples.Count)

$cpuAvg = $samples | Group-Object cpu | ForEach-Object {
    [pscustomobject]@{ cpu = [int]$_.Name; avg = [math]::Round(($_.Group | Measure-Object -Property v -Average).Average, 1) }
} | Sort-Object cpu
Write-Host ("Per-core avg: " + (($cpuAvg | ForEach-Object { "CPU$($_.cpu)=$($_.avg)%" }) -join ' '))

if ($proc -and -not $proc.HasExited) {
    Write-Host "App still running (PID $pidNum), leaving it open"
} else {
    Write-Host "App exited"
}
