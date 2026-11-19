$ErrorActionPreference = 'Stop'

$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$executable = Join-Path $repository 'build\msvc\Debug\poseidon_zero.exe'
$captureDirectory = Join-Path $env:RUNNER_TEMP 'poseidon-screenshots'
$destinationDirectory = Join-Path $repository 'screenshot'
if (-not (Test-Path $executable)) {
    throw "Simulator executable not found: $executable"
}

New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
$env:HOME = $captureDirectory
$screenshotDirectory = Join-Path $captureDirectory 'Pictures\Screenshots'
New-Item -ItemType Directory -Path $screenshotDirectory -Force | Out-Null
$env:POSEIDON_STORE_CAPTURE = '1'

$process = Start-Process -FilePath $executable -WorkingDirectory $repository -PassThru -Wait
if ($process.ExitCode -ne 0) {
    throw "Simulator screenshot mode failed (exit code $($process.ExitCode))."
}

$captures = @(Get-ChildItem -Path $screenshotDirectory -Filter 'poseidon-zero_*.png' |
              Sort-Object LastWriteTimeUtc)
if ($captures.Count -ne 2) {
    throw "Expected two simulator screenshots, found $($captures.Count)."
}

Add-Type -AssemblyName System.Drawing
$destinations = @(
    'poseidon-zero-light-320x170.png',
    'poseidon-zero-dark-320x170.png'
)
for ($index = 0; $index -lt $captures.Count; $index++) {
    $image = [System.Drawing.Image]::FromFile($captures[$index].FullName)
    try {
        if ($image.Width -ne 320 -or $image.Height -ne 170) {
            throw "Unexpected screenshot dimensions: $($image.Width)x$($image.Height)"
        }
    }
    finally {
        $image.Dispose()
    }

    Copy-Item -Path $captures[$index].FullName `
              -Destination (Join-Path $destinationDirectory $destinations[$index]) `
              -Force
}
