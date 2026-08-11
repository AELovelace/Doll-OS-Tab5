param(
    [string]$ArduinoDataPath = (Join-Path $env:LOCALAPPDATA 'Arduino15')
)

$internalPath = Join-Path $ArduinoDataPath 'internal' # Arduino CLI stores profile libraries here.
$libraryDirs = @(Get-ChildItem -LiteralPath $internalPath -Directory -Filter 'M5GFX_0.2.26_*')
if ($libraryDirs.Count -eq 0) {
    throw "M5GFX 0.2.26 was not found under $internalPath"
}

$unpatched = @'
                if (id[0] == 0x71 && id[1] == 0x23) {
                  ESP_LOGI(LIBRARY_NAME, "M5Tab5 ST DSI ID matched 71 23");
                }
'@
$patched = @'
                if (id[0] == 0x71 && id[1] == 0x23) {
                  ESP_LOGI(LIBRARY_NAME, "M5Tab5 ST DSI ID matched 71 23");
                  hit_st7123 = true;
                  break;
                }
'@

$st7123Clock80 = @'
              } else if (hit_st7123) {
                ESP_LOGI(LIBRARY_NAME, "M5Tab5 detected ST7123 display");
                _touch_last.reset(new Touch_ST7123());
                auto p = new Panel_ST7123();
                _panel_last.reset(p);
                auto det = p->config_detail();

                det.dpi_freq_mhz = 80;
'@
$st7123Clock70 = @'
              } else if (hit_st7123) {
                ESP_LOGI(LIBRARY_NAME, "M5Tab5 detected ST7123 display");
                _touch_last.reset(new Touch_ST7123());
                auto p = new Panel_ST7123();
                _panel_last.reset(p);
                auto det = p->config_detail();

                det.dpi_freq_mhz = 70;
'@
$st7123Clock50 = @'
              } else if (hit_st7123) {
                ESP_LOGI(LIBRARY_NAME, "M5Tab5 detected ST7123 display");
                _touch_last.reset(new Touch_ST7123());
                auto p = new Panel_ST7123();
                _panel_last.reset(p);
                auto det = p->config_detail();

                det.dpi_freq_mhz = 50;
'@

$dsiLane1040 = '            bus_cfg.lane_mbps = hit_st7121 ? 900 : 1040;'
$dsiLane1000 = '            bus_cfg.lane_mbps = hit_st7121 ? 900 : 1000;'
$dsiLane800 = '            bus_cfg.lane_mbps = hit_st7121 ? 900 : 800;'

foreach ($libraryDir in $libraryDirs) {
    $sourcePath = Join-Path $libraryDir.FullName 'M5GFX\src\M5GFX.cpp'
    $source = [System.IO.File]::ReadAllText($sourcePath)
    $changed = $false
    if ($source.Contains($patched)) {
        Write-Host "M5GFX Tab5 DSI fallback already patched: $sourcePath"
    } elseif ($source.Contains($unpatched)) {
        $source = $source.Replace($unpatched, $patched)
        $changed = $true
        Write-Host "Patched M5GFX Tab5 DSI fallback: $sourcePath"
    } else {
        throw "Expected Tab5 DSI detection block was not found in $sourcePath"
    }

    if ($source.Contains($st7123Clock50)) {
        Write-Host "M5GFX ST7123 clock already set to 50MHz: $sourcePath"
    } elseif ($source.Contains($st7123Clock70)) {
        $source = $source.Replace($st7123Clock70, $st7123Clock50)
        $changed = $true
        Write-Host "Patched M5GFX ST7123 clock from 70MHz to 50MHz: $sourcePath"
    } elseif ($source.Contains($st7123Clock80)) {
        $source = $source.Replace($st7123Clock80, $st7123Clock50)
        $changed = $true
        Write-Host "Patched M5GFX ST7123 clock from 80MHz to 50MHz: $sourcePath"
    } else {
        throw "Expected M5GFX ST7123 clock block was not found in $sourcePath"
    }

    if ($source.Contains($dsiLane800)) {
        Write-Host "M5GFX Tab5 DSI lane rate already set to 800Mbps: $sourcePath"
    } elseif ($source.Contains($dsiLane1000)) {
        $source = $source.Replace($dsiLane1000, $dsiLane800)
        $changed = $true
        Write-Host "Patched M5GFX Tab5 DSI lane rate from 1000Mbps to 800Mbps: $sourcePath"
    } elseif ($source.Contains($dsiLane1040)) {
        $source = $source.Replace($dsiLane1040, $dsiLane800)
        $changed = $true
        Write-Host "Patched M5GFX Tab5 DSI lane rate from 1040Mbps to 800Mbps: $sourcePath"
    } else {
        throw "Expected M5GFX Tab5 DSI lane-rate block was not found in $sourcePath"
    }

    if ($changed) {
        [System.IO.File]::WriteAllText($sourcePath, $source)
    }
}
