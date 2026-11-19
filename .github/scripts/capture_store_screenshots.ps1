$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PoseidonScreenshotInput
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);
}
'@

$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$executable = Join-Path $repository 'build\msvc\Debug\poseidon_zero.exe'
$captureDirectory = Join-Path $env:RUNNER_TEMP 'poseidon-screenshots'
$destinationDirectory = Join-Path $repository 'screenshot'
$windowTitle = 'POSEIDON for CardputerZero'

if (-not (Test-Path $executable)) {
    throw "Simulator executable not found: $executable"
}

New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
$env:HOME = $captureDirectory
$screenshotDirectory = Join-Path $captureDirectory 'Pictures\Screenshots'
New-Item -ItemType Directory -Path $screenshotDirectory -Force | Out-Null

$process = Start-Process -FilePath $executable -WorkingDirectory $repository -PassThru
try {
    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 40 -and $window -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 250
        $window = [PoseidonScreenshotInput]::FindWindow($null, $windowTitle)
        if ($process.HasExited) {
            throw "Simulator exited before its window appeared (exit code $($process.ExitCode))."
        }
    }

    if ($window -eq [IntPtr]::Zero) {
        throw "Simulator window not found: $windowTitle"
    }

    [void][PoseidonScreenshotInput]::SetForegroundWindow($window)
    Start-Sleep -Seconds 2

    function Send-WindowKey([byte]$key) {
        [PoseidonScreenshotInput]::keybd_event($key, 0, 0, [UIntPtr]::Zero)
        [PoseidonScreenshotInput]::keybd_event($key, 0, 2, [UIntPtr]::Zero)
    }

    function Capture-StoreScreenshot([string]$destination) {
        $existing = @(Get-ChildItem -Path $screenshotDirectory -Filter 'poseidon-zero_*.png' -ErrorAction SilentlyContinue).Count
        Send-WindowKey 0x2c

        $capture = $null
        for ($attempt = 0; $attempt -lt 40 -and -not $capture; $attempt++) {
            Start-Sleep -Milliseconds 250
            $captures = @(Get-ChildItem -Path $screenshotDirectory -Filter 'poseidon-zero_*.png' |
                          Sort-Object LastWriteTimeUtc)
            if ($captures.Count -gt $existing) {
                $capture = $captures[-1]
            }
        }

        if (-not $capture) {
            throw "Simulator did not create a screenshot: $destination"
        }

        Add-Type -AssemblyName System.Drawing
        $image = [System.Drawing.Image]::FromFile($capture.FullName)
        try {
            if ($image.Width -ne 320 -or $image.Height -ne 170) {
                throw "Unexpected screenshot dimensions: $($image.Width)x$($image.Height)"
            }
        }
        finally {
            $image.Dispose()
        }

        Copy-Item -Path $capture.FullName -Destination (Join-Path $destinationDirectory $destination) -Force
    }

    Capture-StoreScreenshot 'poseidon-zero-light-320x170.png'
    Send-WindowKey 0x36
    Start-Sleep -Seconds 1
    Capture-StoreScreenshot 'poseidon-zero-dark-320x170.png'
}
finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id
    }
}
