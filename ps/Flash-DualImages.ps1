[CmdletBinding()]
param(
    [string]$Port = "COM38",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop" # Stops immediately instead of continuing after a failed build or flash.
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path # Anchors every artifact lookup to this repository.
$pioCommand = Get-Command "pio.exe" -ErrorAction SilentlyContinue # Uses the active PlatformIO virtual environment when available.
if (-not $pioCommand) {
    $pioCommand = Get-Command "pio" -ErrorAction SilentlyContinue # Supports shells that expose PlatformIO without the .exe suffix.
}

if ($pioCommand) {
    $pioPath = $pioCommand.Source # Keeps build and esptool dependencies in the active PlatformIO installation.
}
else {
    $pioPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe" # Supports PlatformIO's standard Windows install when PATH is not configured.
    if (-not (Test-Path -LiteralPath $pioPath -PathType Leaf)) {
        throw "PlatformIO was not found on PATH or at $pioPath." # Reports the missing prerequisite before attempting a build or flash.
    }
}

$pioScripts = Split-Path $pioPath -Parent # Locates python.exe beside the PlatformIO executable.
$platformioRoot = (Resolve-Path (Join-Path $pioScripts "..\..")).Path # Locates PlatformIO's installed package directory.
$pythonPath = Join-Path $pioScripts "python.exe" # Uses PlatformIO's Python so esptool dependencies stay compatible.
$esptoolPath = Join-Path $platformioRoot "packages\tool-esptoolpy\esptool.py" # Uses the esptool version pinned by the platform.
$tab5Build = Join-Path $projectRoot ".pio\build\tab5" # Holds the Doll-OS bootloader, table, and application image.
$emulatorBuild = Join-Path $projectRoot ".pio\build\emulator" # Holds the dedicated GBA-only application image.

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Executable @Arguments # Runs the requested build or flashing command without shell-string interpolation.
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Executable $($Arguments -join ' ')" # Prevents a partial install from being reported as successful.
    }
}

function Assert-EspApplication {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$MaximumSize
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing application image: $Path" # Catches absent or failed build outputs before touching flash.
    }

    $image = Get-Item -LiteralPath $Path # Reads the exact generated image size for slot-bound validation.
    if ($image.Length -gt $MaximumSize) {
        throw "Image $Path is $($image.Length) bytes; its slot holds only $MaximumSize bytes." # Blocks writes that would cross a partition boundary.
    }

    $stream = [System.IO.File]::OpenRead($image.FullName) # Reads only the ESP image magic byte instead of loading the whole binary.
    try {
        if ($stream.ReadByte() -ne 0xE9) {
            throw "Image $Path does not have an ESP application header." # Rejects filesystem data and other non-bootable binaries.
        }
    }
    finally {
        $stream.Dispose() # Releases the binary before esptool opens it for flashing.
    }
}

if (-not $SkipBuild) {
    Push-Location $projectRoot # Ensures PlatformIO resolves platformio.ini and partitions.csv from this checkout.
    try {
        Invoke-CheckedCommand -Executable $pioPath -Arguments @("run", "-e", "tab5") # Builds the full Doll-OS image for ota_0.
        Invoke-CheckedCommand -Executable $pioPath -Arguments @("run", "-e", "emulator") # Builds the lean GBA-only image for ota_1.
    }
    finally {
        Pop-Location # Restores the caller's working directory even if a build fails.
    }
}

$bootloaderImage = Join-Path $tab5Build "bootloader.bin" # Boots the shared dual-image partition table.
$partitionImage = Join-Path $tab5Build "partitions.bin" # Declares app0 at 0x10000 and emulator at 0x650000.
$dollOsImage = Join-Path $tab5Build "firmware.bin" # Provides the normal Doll-OS application.
$emulatorImage = Join-Path $emulatorBuild "firmware.bin" # Provides the GBA-only runner application.

foreach ($requiredImage in @($bootloaderImage, $partitionImage)) {
    if (-not (Test-Path -LiteralPath $requiredImage -PathType Leaf)) {
        throw "Missing flash image: $requiredImage" # Stops before opening the serial port when a shared artifact is absent.
    }
}
Assert-EspApplication -Path $dollOsImage -MaximumSize 0x640000 # Confirms Doll-OS fits completely inside ota_0.
Assert-EspApplication -Path $emulatorImage -MaximumSize 0x300000 # Confirms the emulator fits completely inside ota_1.

$env:PYTHONUTF8 = "1" # Prevents esptool progress bars from failing in Windows legacy console encodings.
$flashArguments = @(
    $esptoolPath,
    "--chip", "esp32p4",
    "--port", $Port,
    "--baud", "460800",
    "write-flash", "-z",
    "--flash-mode", "dio",
    "--flash-freq", "80m",
    "--flash-size", "16MB",
    "0x2000", $bootloaderImage,
    "0x8000", $partitionImage,
    "0x10000", $dollOsImage,
    "0x650000", $emulatorImage
) # Writes both applications in one connection while leaving NVS, OTA data, LittleFS, and SD untouched.
Invoke-CheckedCommand -Executable $pythonPath -Arguments $flashArguments # Programs and hash-verifies every supplied flash region.

$verifyArguments = @(
    $esptoolPath,
    "--chip", "esp32p4",
    "--port", $Port,
    "--baud", "460800",
    "verify-flash",
    "0x10000", $dollOsImage,
    "0x650000", $emulatorImage
) # Reconnects and verifies both application slots against their build artifacts.
Invoke-CheckedCommand -Executable $pythonPath -Arguments $verifyArguments # Detects a wrong offset or incomplete second-image upload before handoff.

Write-Host "Dual-image flash verified: Doll-OS @ 0x10000, emulator @ 0x650000." # Gives handlers an unambiguous success result.
