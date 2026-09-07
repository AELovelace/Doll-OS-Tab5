[CmdletBinding()]
param()

$repoRoot = Split-Path -Parent $PSScriptRoot  # Keep the launcher independent of the caller's working directory.
$entryPoint = Join-Path $repoRoot 'python\game_editor_gui.py'  # The required modding-tool entry point stays under /python.
$requirements = Join-Path $repoRoot 'python\requirements.txt'  # This file is the single source for desktop dependencies.

& py -3 -c 'import PySide6' 2>$null  # Check the GUI dependency before showing a confusing import traceback.
if ($LASTEXITCODE -ne 0) {
    Write-Host 'PySide6 is not installed. Run:' -ForegroundColor Yellow
    Write-Host "  py -3 -m pip install -r `"$requirements`""
    exit 1
}

Push-Location $repoRoot  # Start from the repository so logs and relative diagnostics are predictable.
try {
    & py -3 $entryPoint  # Launch the Sakura Flasher and preserve its exit code for scripts or shortcuts.
    exit $LASTEXITCODE
}
finally {
    Pop-Location  # Always return an interactive PowerShell session to the directory it started in.
}
