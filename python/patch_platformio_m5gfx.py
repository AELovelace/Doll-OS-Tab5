from pathlib import Path
import os
import re

Import("env")


def configure_windows_archiver_response_file() -> None:
    """Uses a response file for large ESP-IDF component archives on Windows."""
    if os.name != "nt":
        return

    archive_command = "$AR $ARFLAGS $TARGET $SOURCES"
    env.Replace(ARCOM="${TEMPFILE('%s','$ARCOMSTR')}" % archive_command)  # Avoids Windows' 8191-character limit.
    print("[pio] Enabled response files for large Windows archive commands")


def configure_p4_tinyusb_headers() -> None:
    """Exports the bundled P4 TinyUSB headers required by EspUsbHost."""
    package_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    if not package_dir:
        raise RuntimeError("The Arduino ESP32 precompiled-libraries package is unavailable")

    component_dir = Path(package_dir) / "esp32p4" / "include" / "arduino_tinyusb"
    tinyusb_dir = component_dir / "tinyusb" / "src"
    config_dir = component_dir / "include"
    if not (tinyusb_dir / "class" / "hid" / "hid.h").is_file():
        raise RuntimeError(f"The ESP32-P4 TinyUSB headers are missing beneath {tinyusb_dir}")
    if not (config_dir / "tusb_config.h").is_file():
        raise RuntimeError(f"The ESP32-P4 TinyUSB configuration is missing beneath {config_dir}")

    env.AppendUnique(CPPPATH=[str(tinyusb_dir), str(config_dir)])  # Exports TinyUSB APIs and its target config.
    print(f"[pio] Exported ESP32-P4 TinyUSB headers: {component_dir}")


def patch_m5gfx(*_args, **_kwargs) -> None:
    """Fixes ST7123 fallback detection while preserving M5GFX's native DSI timings."""
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    candidates = list(libdeps_dir.glob("*/M5GFX/src/M5GFX.cpp"))
    if not candidates:
        raise RuntimeError(f"M5GFX source was not installed beneath {libdeps_dir}")

    source_path = candidates[0]
    source = source_path.read_text(encoding="utf-8")
    original = source

    unmatched_detection = """                if (id[0] == 0x71 && id[1] == 0x23) {
                  ESP_LOGI(LIBRARY_NAME, \"M5Tab5 ST DSI ID matched 71 23\");
                }
"""
    matched_detection = """                if (id[0] == 0x71 && id[1] == 0x23) {
                  ESP_LOGI(LIBRARY_NAME, \"M5Tab5 ST DSI ID matched 71 23\");
                  hit_st7123 = true;
                  break;
                }
"""
    if unmatched_detection in source:
        source = source.replace(unmatched_detection, matched_detection)
    elif matched_detection not in source:
        raise RuntimeError("Expected M5GFX ST7123 detection block was not found")

    #Undo the earlier diagnostic underclock. Panel timings are deliberately matched to
    #M5GFX 0.2.26: ILI9881C/ST7123 at 80MHz, ST7121 at 70MHz, and 1040Mbps for
    #the non-ST7121 DSI path.
    native_panel_clocks = (
        ("Panel_ILI9881C", 80),
        ("Panel_ST7121", 70),
        ("Panel_ST7123", 80),
    )
    for panel_name, native_clock in native_panel_clocks:
        timing_pattern = re.compile(
            rf"(auto p = new {panel_name}\(\);\s+"
            rf"_panel_last\.reset\(p\);\s+"
            rf"auto det = p->config_detail\(\);\s+"
            rf"det\.dpi_freq_mhz = )\d+;"
        )
        source, match_count = timing_pattern.subn(rf"\g<1>{native_clock};", source, count=1)
        if match_count != 1:
            raise RuntimeError(f"Expected one M5GFX timing block for {panel_name}, found {match_count}")

    source = source.replace(
        "bus_cfg.lane_mbps = hit_st7121 ? 900 : 800;",
        "bus_cfg.lane_mbps = hit_st7121 ? 900 : 1040;",
    )
    if "bus_cfg.lane_mbps = hit_st7121 ? 900 : 1040;" not in source:
        raise RuntimeError("Expected native M5GFX 1040Mbps DSI lane timing was not found")

    if source != original:
        source_path.write_text(source, encoding="utf-8")
        print(f"[pio] Patched Tab5 M5GFX detection/native timings: {source_path}")
    else:
        print(f"[pio] Tab5 M5GFX detection/native timings already active: {source_path}")


def patch_esp_usb_host() -> None:
    """Keeps EspUsbHost compatible with the UserDemo-matched ESP-IDF 5.4 API."""
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    candidates = list(libdeps_dir.glob("*/EspUsbHost/src/EspUsbHost.cpp"))
    if not candidates:
        raise RuntimeError(f"EspUsbHost source was not installed beneath {libdeps_dir}")

    source_path = candidates[0]
    source = source_path.read_text(encoding="utf-8")
    original = source
    idf_55_assignment = """#if defined(CONFIG_IDF_TARGET_ESP32P4)
  hostConfig.peripheral_map = hostPeripheralMap(config_.port);
#endif
"""
    versioned_assignment = """#if defined(CONFIG_IDF_TARGET_ESP32P4) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  hostConfig.peripheral_map = hostPeripheralMap(config_.port);
#endif
"""
    if idf_55_assignment in source:
        source = source.replace(idf_55_assignment, versioned_assignment)
    elif versioned_assignment not in source:
        raise RuntimeError("Expected EspUsbHost ESP32-P4 peripheral selection block was not found")

    if '#include "esp_idf_version.h"' not in source:
        source = source.replace('#include "EspUsbHostHid.h"\n', '#include "EspUsbHostHid.h"\n#include "esp_idf_version.h"\n')

    if source != original:
        source_path.write_text(source, encoding="utf-8")
        print(f"[pio] Patched EspUsbHost for ESP-IDF 5.4: {source_path}")
    else:
        print(f"[pio] EspUsbHost ESP-IDF 5.4 patch already active: {source_path}")


def patch_esp32_audio_i2s() -> None:
    """Uses the Tab5 speaker backend's native 128x MCLK instead of 384x."""
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    candidates = list(libdeps_dir.glob("*/ESP32-audioI2S/src/Audio.cpp"))
    if not candidates:
        raise RuntimeError(f"ESP32-audioI2S source was not installed beneath {libdeps_dir}")

    source_path = candidates[0]
    source = source_path.read_text(encoding="utf-8")
    original = source
    upstream_clock = "m_i2s_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;"
    tab5_clock = "m_i2s_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;"

    if upstream_clock in source:
        if source.count(upstream_clock) != 1:
            raise RuntimeError("Expected exactly one ESP32-audioI2S 384x MCLK setting")
        source = source.replace(upstream_clock, tab5_clock)
    elif tab5_clock not in source:
        raise RuntimeError("Expected ESP32-audioI2S MCLK setting was not found")

    if source != original:
        source_path.write_text(source, encoding="utf-8")
        print(f"[pio] Patched ESP32-audioI2S for Tab5 128x MCLK: {source_path}")
    else:
        print(f"[pio] ESP32-audioI2S Tab5 128x MCLK already active: {source_path}")


def verify_tab5_sdkconfig(source, target, env) -> None:
    """Rejects display, watchdog, memory-placement, or GBA-JIT regressions."""
    del source, target  # SCons supplies these action arguments, but this check only needs the environment.
    config_path = Path(env.subst("$BUILD_DIR")) / "config" / "sdkconfig.h"
    config = config_path.read_text(encoding="utf-8")
    required = (
        "#define CONFIG_COMPILER_OPTIMIZATION_PERF 1",
        "#define CONFIG_SPIRAM_SPEED_200M 1",
        "#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1",
        #128KB, not 256KB: the L2 cache is subtracted from internal SRAM by
        #memory.ld.in, and the larger cache cost 128KB of the pool this firmware
        #needs at runtime. The 128-byte line is what keeps scanout refills wide.
        "#define CONFIG_CACHE_L2_CACHE_128KB 1",
        "#define CONFIG_CACHE_L2_CACHE_LINE_128B 1",
        "#define CONFIG_ESP_TASK_WDT_EN 1",
    )
    missing = [setting for setting in required if setting not in config]
    if missing:
        raise RuntimeError(f"Unsafe Tab5 display configuration in {config_path}: {missing}")

    #WiFi/Hosted buffers and task stacks must remain internal: putting sustained
    #STA traffic in PSRAM competes with the DSI driver's continuous framebuffer DMA.
    forbidden = (
        "#define CONFIG_ESP_TASK_WDT_INIT 1",
        "#define CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP 1",
        "#define CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM 1",
        "#define CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM 1",
        #P4's PMP IRAM/DRAM split removes MALLOC_CAP_EXEC from all L2 heap.
        #gpSP needs a small writable/executable arena for its Thumb dynarec.
        "#define CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT 1",
    )
    enabled = [setting for setting in forbidden if setting in config]
    if enabled:
        raise RuntimeError(f"Unsafe Tab5 runtime configuration in {config_path}: {enabled}")

    print("[pio] Verified display bandwidth, GBA executable heap, internal WiFi/Hosted memory, and inactive task watchdog")


configure_windows_archiver_response_file()
configure_p4_tinyusb_headers()
patch_m5gfx()
patch_esp_usb_host()
patch_esp32_audio_i2s()
env.AddPostAction("$PROGPATH", verify_tab5_sdkconfig)  # Prevents flashing a silent low-bandwidth fallback.
