# Windows counterpart of tools/run-linux.sh: asset preflight, default paths
# inside the checkout, then the game. Extra arguments go to gow3.exe.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'build-windows\bin\gow3.exe'
if (-not (Test-Path $exe)) { throw "Missing $exe; build it with tools/build-windows.sh." }

function Find-Python {
    foreach ($candidate in @(@('python'), @('py', '-3'))) {
        if (Get-Command $candidate[0] -ErrorAction SilentlyContinue) { return ,$candidate }
    }
    return $null
}

# Honor a custom asset directory for preflight as well as the actual mount.
$gameDataRoot = Join-Path $root 'game'
$checkAssets = $true
for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = [string]$args[$i]
    if ($arg -eq '--game_data_root') {
        if ($i + 1 -ge $args.Count) { throw 'The --game_data_root option requires an asset directory.' }
        $gameDataRoot = [string]$args[$i + 1]
    } elseif ($arg.StartsWith('--game_data_root=')) {
        $gameDataRoot = $arg.Substring('--game_data_root='.Length)
    } elseif ($arg -in @('--help', '-h', '--version')) {
        $checkAssets = $false
    }
}
if ($checkAssets) {
    $python = Find-Python
    if ($python) {
        $pythonArgs = @($python | Select-Object -Skip 1) + @('tools/import-assets.py', 'check', '--output', $gameDataRoot)
        & $python[0] @pythonArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error ('Import your own ISO first: python tools/import-assets.py import "C:\path\to\gow3.iso"' +
                "`nFor an existing extraction, add --adopt-existing; for a custom directory, add --output `"C:\path\to\game`".")
            exit 1
        }
    } else {
        Write-Warning 'Python not found; skipping the asset check.'
    }
}
New-Item -ItemType Directory -Force -Path (Join-Path $root 'user'), (Join-Path $root 'logs') | Out-Null

$defaults = [ordered]@{
    '--game_data_root' = $gameDataRoot
    '--user_data_root' = (Join-Path $root 'user')
    '--log_file'       = (Join-Path $root 'logs\gow3.log')
    '--gow3_config'    = (Join-Path $root 'config\gow3.toml')
}
# CLI11 rejects repeated options. Omit a default when the caller supplies it.
$gameArgs = @()
foreach ($option in $defaults.Keys) {
    $overridden = $args | Where-Object { $_ -eq $option -or ([string]$_).StartsWith("$option=") }
    if (-not $overridden) { $gameArgs += "$option=$($defaults[$option])" }
}
$gameArgs += $args
$process = Start-Process -FilePath $exe -ArgumentList ($gameArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -WorkingDirectory $root -PassThru -Wait
exit $process.ExitCode
