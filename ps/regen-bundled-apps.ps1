param()

$ErrorActionPreference = 'Stop'

$adventurePath = Join-Path $PSScriptRoot '..\apps\adventure.dapp'
$tetrisPath = Join-Path $PSScriptRoot '..\apps\tetris.dapp'
$snakePath = Join-Path $PSScriptRoot '..\apps\snake.dapp'
$chatPath = Join-Path $PSScriptRoot '..\apps\dappchat.dapp'
$storePath = Join-Path $PSScriptRoot '..\apps\dappstore.dapp'
$docSrcPath = Join-Path $PSScriptRoot '..\docs\DAPP.txt'
$outPath = Join-Path $PSScriptRoot '..\BundledApps.h'

$utf8 = [System.Text.Encoding]::UTF8
$adventureSrc = [System.IO.File]::ReadAllText($adventurePath, $utf8)
$tetrisSrc = [System.IO.File]::ReadAllText($tetrisPath, $utf8)
$snakeSrc = [System.IO.File]::ReadAllText($snakePath, $utf8)
$chatSrc = [System.IO.File]::ReadAllText($chatPath, $utf8)
$storeSrc = [System.IO.File]::ReadAllText($storePath, $utf8)
$docSrc = [System.IO.File]::ReadAllText($docSrcPath, $utf8)

$header = "#pragma once`r`n`r`n"
$header += "// Generated from apps/adventure.dapp for firmware-side LittleFS seeding.`r`n"
$header += "static const char BUNDLED_APP_ADVENTURE[] = R`"DOLLAPP(" + $adventureSrc + ")DOLLAPP`";`r`n`r`n"
$header += "// Generated from apps/tetris.dapp for firmware-side LittleFS seeding.`r`n"
$header += "static const char BUNDLED_APP_TETRIS[] = R`"DOLLAPP(" + $tetrisSrc + ")DOLLAPP`";`r`n`r`n"
$header += "// Generated from apps/snake.dapp for firmware-side LittleFS seeding.`r`n"
$header += "static const char BUNDLED_APP_SNAKE[] = R`"DOLLAPP(" + $snakeSrc + ")DOLLAPP`";`r`n`r`n"
$header += "// Generated from apps/dappchat.dapp for firmware-side LittleFS seeding.`r`n"
$header += "static const char BUNDLED_APP_DAPPCHAT[] = R`"DOLLAPP(" + $chatSrc + ")DOLLAPP`";`r`n`r`n"
$header += "// Generated from apps/dappstore.dapp for firmware-side LittleFS seeding.`r`n"
$header += "static const char BUNDLED_APP_DAPPSTORE[] = R`"DOLLAPP(" + $storeSrc + ")DOLLAPP`";`r`n`r`n"
$header += "// Generated from docs/DAPP.txt for firmware-side LittleFS seeding.`r`n"
$header += "static const char BUNDLED_DOC_DAPP[] = R`"DOLLDOC(" + $docSrc + ")DOLLDOC`";`r`n"

[System.IO.File]::WriteAllText($outPath, $header, [System.Text.UTF8Encoding]::new($false))
