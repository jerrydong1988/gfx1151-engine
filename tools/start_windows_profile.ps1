# Start a tested arithmetic profile without rewriting service.conf or user environment.
[CmdletBinding()]
param(
    [ValidateSet('v2-exact', 'v2-fork', 'v2-hybrid', 'w4b')]
    [string]$Profile = 'v2-exact',
    [string]$ModelDirectory,
    [string]$Root,
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion -lt [version]'5.1') { throw 'PowerShell 5.1+ required' }
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path
if (-not $ModelDirectory) {
    $ModelDirectory = $env:MODEL_DIR
    if (-not $ModelDirectory) {
        # Only accept a literal MODEL_DIR. Never execute/source the configuration.
        $line = Get-Content -LiteralPath (Join-Path $Root 'service.conf') |
            Where-Object { $_ -match '^\s*MODEL_DIR\s*=' } | Select-Object -Last 1
        if ($line -match '^\s*MODEL_DIR\s*=\s*["'']([^$"'']+)["'']\s*$') {
            $ModelDirectory = $Matches[1]
        } else { throw 'Pass -ModelDirectory when service.conf has no literal MODEL_DIR' }
    }
}
if (-not [System.IO.Path]::IsPathRooted($ModelDirectory)) {
    $ModelDirectory = Join-Path $Root $ModelDirectory
}
$ModelDirectory = (Resolve-Path -LiteralPath $ModelDirectory).Path
$profiles = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows_profiles.json') -Raw |
    ConvertFrom-Json
$selected = $profiles.profiles.PSObject.Properties[$Profile].Value
$launch = [System.Diagnostics.ProcessStartInfo]::new()
$launch.FileName = Join-Path $Root 'start_win.exe'
$launch.WorkingDirectory = $Root
$launch.UseShellExecute = $false
$launch.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$launch.CreateNoWindow = $true
# Remove inherited research switches only in this child. Native launcher restores
# its normal production flags, then the selected explicit policy is applied.
foreach ($key in @($launch.EnvironmentVariables.Keys)) {
    if ($key.StartsWith('GDEC_')) { [void]$launch.EnvironmentVariables.Remove($key) }
}
foreach ($settings in @($profiles.common, $selected)) {
    foreach ($entry in $settings.PSObject.Properties) {
        $launch.EnvironmentVariables[$entry.Name] = [string]$entry.Value
    }
}
$launch.EnvironmentVariables['MODEL_DIR'] = $ModelDirectory
foreach ($key in @('MODEL_FILE', 'NGRAM_FILE', 'MTP_FILE', 'OVERLAY_FILE', 'VISION_FILE', 'TOKENIZER_DIR')) {
    if ($launch.EnvironmentVariables.ContainsKey($key) -and $launch.EnvironmentVariables[$key]) {
        $launch.EnvironmentVariables[$key] = Join-Path $ModelDirectory $launch.EnvironmentVariables[$key]
    }
}
if ($Check) {
    $launch.Arguments = '--check'
    $launch.RedirectStandardOutput = $true
    $launch.RedirectStandardError = $true
    $launch.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $launch.StandardErrorEncoding = [System.Text.Encoding]::UTF8
}
$process = [System.Diagnostics.Process]::Start($launch)
if ($Check) {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        # This is this script's own dry-run child, never an existing server.
        $process.Kill()
        throw 'Launcher configuration check timed out'
    }
    Write-Output $stdout.GetAwaiter().GetResult()
    Write-Output $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "Launcher check failed: $($process.ExitCode)" }
} else {
    Write-Output "Started $Profile launcher PID $($process.Id). Read the tray/logs for readiness."
}
$process.Dispose()
