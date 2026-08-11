from pathlib import Path
import os

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
    """Applies the tested ST7123 detection and conservative DSI timing patch."""
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

    for old_clock in ("det.dpi_freq_mhz = 80;", "det.dpi_freq_mhz = 70;"):
        source = source.replace(old_clock, "det.dpi_freq_mhz = 50;")
    for old_lane in (
        "bus_cfg.lane_mbps = hit_st7121 ? 900 : 1040;",
        "bus_cfg.lane_mbps = hit_st7121 ? 900 : 1000;",
    ):
        source = source.replace(old_lane, "bus_cfg.lane_mbps = hit_st7121 ? 900 : 800;")

    if "det.dpi_freq_mhz = 50;" not in source:
        raise RuntimeError("Expected M5GFX ST7123 50MHz timing was not found")
    if "bus_cfg.lane_mbps = hit_st7121 ? 900 : 800;" not in source:
        raise RuntimeError("Expected M5GFX 800Mbps DSI lane timing was not found")

    if source != original:
        source_path.write_text(source, encoding="utf-8")
        print(f"[pio] Patched Tab5 M5GFX source: {source_path}")
    else:
        print(f"[pio] Tab5 M5GFX patch already active: {source_path}")


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


def verify_tab5_sdkconfig(source, target, env) -> None:
    """Rejects a firmware image if any display-bandwidth setting was dropped."""
    del source, target  # SCons supplies these action arguments, but this check only needs the environment.
    config_path = Path(env.subst("$BUILD_DIR")) / "config" / "sdkconfig.h"
    config = config_path.read_text(encoding="utf-8")
    required = (
        "#define CONFIG_COMPILER_OPTIMIZATION_PERF 1",
        "#define CONFIG_SPIRAM_SPEED_200M 1",
        "#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1",
        "#define CONFIG_CACHE_L2_CACHE_256KB 1",
        "#define CONFIG_CACHE_L2_CACHE_LINE_128B 1",
    )
    missing = [setting for setting in required if setting not in config]
    if missing:
        raise RuntimeError(f"Unsafe Tab5 display configuration in {config_path}: {missing}")

    print("[pio] Verified 200MHz PSRAM, PSRAM XIP, and the 256KB/128-byte L2 cache")


configure_windows_archiver_response_file()
configure_p4_tinyusb_headers()
patch_m5gfx()
patch_esp_usb_host()
env.AddPostAction("$PROGPATH", verify_tab5_sdkconfig)  # Prevents flashing a silent low-bandwidth fallback.
