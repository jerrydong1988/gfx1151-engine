<#
Read-only, bounded Windows I/O sampling while an existing inference runs.
Example: ./tools/capture_prefill_io.ps1 -OutPath logs/prefill-io.jsonl -DurationSeconds 120
No model requests, cache flushing, service restarts or configuration changes.
Process I/O includes files/devices/network; page faults are not proof of disk I/O.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutPath,
    [ValidateRange(1,3600)][int]$DurationSeconds = 120,
    [ValidateRange(1,30)][int]$IntervalSeconds = 2
)
$ErrorActionPreference = 'Stop'
$destination = [IO.Path]::GetFullPath($OutPath)
if (Test-Path -LiteralPath $destination) { throw 'Output exists; choose a new path to preserve evidence.' }
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
$writer = [IO.StreamWriter]::new([IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read))
$writer.AutoFlush = $true
$watch = [Diagnostics.Stopwatch]::StartNew()
$previousDisks = @{}
$previousProcesses = @{}
$previousMemory = $null
$samples = 0
function Rate($Current, $Previous, [string]$Field) {
    if (!$Previous) { return $null }
    $seconds = ([double]$Current.Timestamp_PerfTime - [double]$Previous.Timestamp_PerfTime) / [double]$Current.Frequency_PerfTime
    $delta = [double]$Current.$Field - [double]$Previous.$Field
    if ($seconds -le 0 -or $delta -lt 0) { return $null }
    return [Math]::Round($delta / $seconds, 3)
}
try {
    $writer.WriteLine((@{schema=1; type='metadata'; started_utc=[DateTime]::UtcNow.ToString('o'); duration_seconds=$DurationSeconds; interval_seconds=$IntervalSeconds; semantics='Raw Windows performance counters converted by counter timestamps. First rates are null; observations are system-wide, not attributed to an engine request. Process I/O includes network/devices; page input is system-wide. Current samples cannot reconstruct a previous incident. No prompts or command lines captured.'} | ConvertTo-Json -Compress))
    do {
        $errors = @()
        $diskRows = @()
        $processRows = @()
        $memoryRow = $null
        $utc = [DateTime]::UtcNow.ToString('o')
        try {
            $disks = @(Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk -OperationTimeoutSec 5 | Where-Object Name -ne '_Total')
            foreach ($disk in $disks) {
                $previous = $previousDisks[$disk.Name]
                $latency = $null
                if ($previous) {
                    $reads = [double]$disk.AvgDisksecPerRead_Base - [double]$previous.AvgDisksecPerRead_Base
                    $ticks = [double]$disk.AvgDisksecPerRead - [double]$previous.AvgDisksecPerRead
                    if ($reads -gt 0 -and $ticks -ge 0) { $latency = [Math]::Round($ticks / [double]$disk.Frequency_PerfTime / $reads * 1000, 3) }
                }
                $diskRows += @{name=$disk.Name; read_bytes_per_sec=(Rate $disk $previous 'DiskReadBytesPersec'); reads_per_sec=(Rate $disk $previous 'DiskReadsPersec'); avg_read_ms=$latency; queue_length=[long]$disk.CurrentDiskQueueLength}
                $previousDisks[$disk.Name] = $disk
            }
        } catch { $errors += 'disk counters: ' + $_.Exception.Message }
        try {
            $memory = Get-CimInstance Win32_PerfRawData_PerfOS_Memory -OperationTimeoutSec 5
            $memoryRow = @{available_mib=[long]$memory.AvailableMBytes; cache_mib=[Math]::Round([double]$memory.CacheBytes / 1MB, 1); pages_input_per_sec=(Rate $memory $previousMemory 'PagesInputPersec'); page_reads_per_sec=(Rate $memory $previousMemory 'PageReadsPersec')}
            $previousMemory = $memory
        } catch { $errors += 'memory counters: ' + $_.Exception.Message }
        try {
            $processes = @(Get-CimInstance Win32_PerfRawData_PerfProc_Process -OperationTimeoutSec 5 | Where-Object { $_.IDProcess -gt 0 -and $_.Name -ne '_Total' })
            $next = @{}
            foreach ($process in $processes) {
                $identity = '{0}:{1}' -f $process.IDProcess,$process.ElapsedTime
                $previous = $previousProcesses[$identity]
                $processRows += @{pid=[long]$process.IDProcess; name=$process.Name; io_read_bytes_per_sec=(Rate $process $previous 'IOReadBytesPersec'); io_write_bytes_per_sec=(Rate $process $previous 'IOWriteBytesPersec'); working_set_mib=[Math]::Round([double]$process.WorkingSet / 1MB, 1)}
                $next[$identity] = $process
            }
            $previousProcesses = $next
            $processRows = @($processRows | Sort-Object { $_.io_read_bytes_per_sec } -Descending | Select-Object -First 10)
        } catch { $errors += 'process counters: ' + $_.Exception.Message }
        $writer.WriteLine((@{type='sample'; utc=$utc; elapsed_seconds=[Math]::Round($watch.Elapsed.TotalSeconds, 3); disks=$diskRows; memory=$memoryRow; top_process_io=$processRows; errors=$errors} | ConvertTo-Json -Depth 8 -Compress))
        $samples++
        $remaining = $DurationSeconds - $watch.Elapsed.TotalSeconds
        if ($remaining -le $IntervalSeconds) {
            if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($remaining * 1000)) }
            break
        }
        Start-Sleep -Seconds $IntervalSeconds
    } while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds)
} finally {
    $writer.Dispose()
}
Write-Output "Captured $samples samples to $destination"
