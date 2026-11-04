param(
    [int]$DurationSeconds = 20
)

Write-Host "=== InstantReplay Performance Benchmark ($DurationSeconds seconds) ===" -ForegroundColor Cyan

$proc = Get-Process InstantReplay -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Host "InstantReplay.exe is not currently running. Starting it now..." -ForegroundColor Yellow
    Start-Process -FilePath "$PSScriptRoot\build\InstantReplay.exe" -WorkingDirectory $PSScriptRoot
    Start-Sleep -Seconds 2
    $proc = Get-Process InstantReplay -ErrorAction SilentlyContinue
    if (-not $proc) {
        Write-Error "Failed to start InstantReplay.exe"
        exit 1
    }
}

$logicalCores = [int]$env:NUMBER_OF_PROCESSORS
Write-Host ("Monitoring PID {0} across {1} logical CPU cores..." -f $proc.Id, $logicalCores) -ForegroundColor Gray

# Start nvidia-smi dmon in background to sample GPU 3D (sm), NVENC (enc), NVDEC (dec), and Memory bw (mem)
$nvLog = Join-Path $env:TEMP "nv_dmon_bench.txt"
if (Test-Path $nvLog) { Remove-Item $nvLog -Force }
$nvProc = Start-Process -FilePath "nvidia-smi" -ArgumentList "dmon -s u -d 1 -c $DurationSeconds" `
    -RedirectStandardOutput $nvLog -NoNewWindow -PassThru -ErrorAction SilentlyContinue

$cpuSamples = @()
$wsSamples  = @()
$privSamples = @()

$prevCpuSec = $proc.TotalProcessorTime.TotalSeconds
$sw = [System.Diagnostics.Stopwatch]::StartNew()

for ($i = 1; $i -le $DurationSeconds; $i++) {
    Start-Sleep -Seconds 1
    $proc.Refresh()
    if ($proc.HasExited) { break }

    $curCpuSec = $proc.TotalProcessorTime.TotalSeconds
    $elapsedSec = $sw.Elapsed.TotalSeconds
    $sw.Restart()

    $cpuPct = (($curCpuSec - $prevCpuSec) / ($elapsedSec * $logicalCores)) * 100.0
    $prevCpuSec = $curCpuSec

    $wsMB   = [math]::Round($proc.WorkingSet64 / 1MB, 1)
    $privMB = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)

    $cpuSamples  += $cpuPct
    $wsSamples   += $wsMB
    $privSamples += $privMB

    Write-Host ("[{0,2}s] CPU: {1,5:N2}% | WorkingSet: {2,6:N1} MB | PrivateRAM: {3,6:N1} MB" -f $i, $cpuPct, $wsMB, $privMB)
}

if ($nvProc -and -not $nvProc.HasExited) {
    $nvProc.WaitForExit(2000) | Out-Null
}

$smSamples  = @()
$encSamples = @()
$memSamples = @()

if (Test-Path $nvLog) {
    Get-Content $nvLog | ForEach-Object {
        $line = $_.Trim()
        if ($line -and -not $line.StartsWith("#")) {
            $parts = $line -split '\s+'
            if ($parts.Count -ge 5) {
                $smSamples  += [double]$parts[1]
                $memSamples += [double]$parts[2]
                $encSamples += [double]$parts[3]
            }
        }
    }
}

function Get-Stats($arr) {
    if (-not $arr -or $arr.Count -eq 0) { return @{ Avg = 0; Max = 0; Min = 0 } }
    $m = $arr | Measure-Object -Average -Maximum -Minimum
    return @{ Avg = $m.Average; Max = $m.Maximum; Min = $m.Minimum }
}

$cpuStat  = Get-Stats $cpuSamples
$wsStat   = Get-Stats $wsSamples
$privStat = Get-Stats $privSamples
$smStat   = Get-Stats $smSamples
$encStat  = Get-Stats $encSamples
$memStat  = Get-Stats $memSamples

Write-Host "`n================ SUMMARY REPORT ================" -ForegroundColor Green
Write-Host ("Process CPU Usage     : Avg {0:N2}% | Max {1:N2}% (across {2} threads)" -f $cpuStat.Avg, $cpuStat.Max, $logicalCores)
Write-Host ("Process Working Set   : Avg {0:N1} MB | Max {1:N1} MB" -f $wsStat.Avg, $wsStat.Max)
Write-Host ("Process Private RAM   : Avg {0:N1} MB | Max {1:N1} MB" -f $privStat.Avg, $privStat.Max)
Write-Host ("GPU 3D Shader (SM)    : Avg {0:N1}%  | Max {1:N1}%" -f $smStat.Avg, $smStat.Max)
Write-Host ("GPU NVENC Engine      : Avg {0:N1}%  | Max {1:N1}%" -f $encStat.Avg, $encStat.Max)
Write-Host ("GPU Memory Bandwidth  : Avg {0:N1}%  | Max {1:N1}%" -f $memStat.Avg, $memStat.Max)

$logFile = Join-Path $PSScriptRoot "instant_replay.log"
if (Test-Path $logFile) {
    $perfLines = Get-Content $logFile | Where-Object { $_ -match "\[PERF\]" } | Select-Object -Last 4
    if ($perfLines) {
        Write-Host "`n--- Internal Microsecond Frame Telemetry (from instant_replay.log) ---" -ForegroundColor Cyan
        $perfLines | ForEach-Object { Write-Host $_ }
    }
}
Write-Host "================================================" -ForegroundColor Green
