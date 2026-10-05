"""Testable configuration and command-building helpers for Sakura Flasher."""

from __future__ import annotations

import os
import re
import shutil
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Mapping


class FirmwareTarget(str, Enum):
    """Identifies which ESP32-S3 firmware project is being operated on."""

    MAIN = "doll-os"
    SLAVE = "ds-slave"


class FlashAction(str, Enum):
    """Identifies whether Arduino CLI should only build or also upload."""

    BUILD = "build"
    FLASH = "flash"


@dataclass(frozen=True)
class ProjectPaths:
    """Holds every repository path needed by the desktop application."""

    root: Path
    main_sketch: Path
    slave_sketch: Path
    build_root: Path


MAIN_VARIANTS = {
    "FNK0104AB 2.8-inch ILI9341": "FNK0104AB_2P8_240x320_ILI9341",
    "FNK0104N 3.5-inch ST77922": "FNK0104N_3P5_320x480_ST77922",
    "FNK0104S 4.0-inch ST7796": "FNK0104S_4P0_320x480_ST7796",
}

MAIN_STRING_FIELDS = (
    "STA_DEFAULT_SSID",
    "STA_DEFAULT_PASSWORD",
    "FTP_USER",
    "FTP_PASS",
    "ASUKA_BRAVE_API_KEY",
    "ASUKA_OPENWEATHER_API_KEY",
)

SLAVE_BOOLEAN_FIELDS = (
    "SLAVE_ROTARY_REVERSED",
    "SLAVE_JOYSTICK_INVERT_X",
    "SLAVE_JOYSTICK_INVERT_Y",
    "SLAVE_SELECT_IS_FOURTH_BUTTON",
    "SLAVE_BUTTON_BAR_ACTIVE_LOW",
)


def discover_project_paths(module_file: Path | None = None) -> ProjectPaths:
    """Finds the DOLL-OS and neighboring DS-Slave projects from this package."""

    source = (module_file or Path(__file__)).resolve()  # Resolve symlinks before walking to the repository root.
    root = source.parents[2]  # core.py lives under <repo>/python/sakura_flasher/.
    return ProjectPaths(
        root=root,
        main_sketch=root / "firmware" / "DS",
        slave_sketch=root.parent / "DS-Slave",
        build_root=root / ".arduino-build" / "sakura-flasher",
    )


def _candidate_cli_paths(root: Path) -> list[Path]:
    """Returns common Arduino CLI locations in priority order."""

    executable = "arduino-cli.exe" if os.name == "nt" else "arduino-cli"
    candidates = [root / "tools" / executable]  # A future bundled CLI always wins over machine-wide installs.
    if os.name == "nt":
        local_app_data = Path(os.environ.get("LOCALAPPDATA", ""))
        program_files = Path(os.environ.get("ProgramFiles", "C:/Program Files"))
        relative = Path("Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe")
        candidates.extend((local_app_data / "Programs" / relative, program_files / relative))
    return candidates


def find_arduino_cli(root: Path, preferred: str = "") -> str:
    """Locates Arduino CLI from user settings, environment, PATH, or Arduino IDE."""

    supplied = preferred.strip() or os.environ.get("ARDUINO_CLI", "").strip()
    if supplied and Path(supplied).is_file():
        return str(Path(supplied).resolve())
    on_path = shutil.which("arduino-cli")
    if on_path:
        return str(Path(on_path).resolve())
    for candidate in _candidate_cli_paths(root):
        if candidate.is_file():
            return str(candidate.resolve())
    return ""


def cpp_escape(value: str) -> str:
    """Escapes a Python value for a double-quoted C++ string literal."""

    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "\\r").replace("\n", "\\n")


def cpp_unescape(value: str) -> str:
    """Decodes the small C++ escape subset emitted by cpp_escape."""

    result: list[str] = []
    index = 0
    while index < len(value):
        if value[index] != "\\" or index + 1 >= len(value):
            result.append(value[index])
            index += 1
            continue
        escaped = value[index + 1]
        result.append({"n": "\n", "r": "\r", "\\": "\\", '"': '"'}.get(escaped, escaped))
        index += 2
    return "".join(result)


def read_cpp_strings(path: Path, names: tuple[str, ...] = MAIN_STRING_FIELDS) -> dict[str, str]:
    """Reads selected const-char string values from a DOLL-OS config header."""

    if not path.is_file():
        return {}
    text = path.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    for name in names:
        pattern = rf"\b{re.escape(name)}\s*=\s*\"((?:\\.|[^\"\\])*)\"\s*;"
        match = re.search(pattern, text)
        if match:
            values[name] = cpp_unescape(match.group(1))
    return values


def replace_cpp_string(text: str, name: str, value: str) -> str:
    """Replaces one named C++ string assignment without exposing other secrets."""

    pattern = rf"(\b{re.escape(name)}\s*=\s*)\"(?:\\.|[^\"\\])*\"(\s*;)"
    replacement = lambda match: f'{match.group(1)}"{cpp_escape(value)}"{match.group(2)}'
    updated, count = re.subn(pattern, replacement, text, count=1)
    if count != 1:
        raise ValueError(f"Could not find {name} in the configuration template.")
    return updated


def _atomic_write(path: Path, text: str) -> None:
    """Writes configuration through a sibling temporary file to avoid truncation."""

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".sakura.tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def write_main_config(sketch: Path, values: Mapping[str, str]) -> Path:
    """Creates or updates config.h from the checked-in example template."""

    destination = sketch / "config.h"
    template = destination if destination.is_file() else sketch / "config.h.example"
    if not template.is_file():
        raise FileNotFoundError(f"Missing DOLL-OS configuration template: {template}")
    text = template.read_text(encoding="utf-8")
    for name in MAIN_STRING_FIELDS:
        if name in values:
            text = replace_cpp_string(text, name, values[name])
    _atomic_write(destination, text)
    return destination


def read_main_variant(path: Path) -> tuple[str, bool]:
    """Reads the selected panel macro and upside-down display flag."""

    if not path.is_file():
        return next(iter(MAIN_VARIANTS.values())), False
    text = path.read_text(encoding="utf-8")
    selected = next(iter(MAIN_VARIANTS.values()))
    for macro in MAIN_VARIANTS.values():
        if re.search(rf"(?m)^\s*#define\s+{re.escape(macro)}\s*$", text):
            selected = macro
            break
    upside_down = bool(re.search(r"(?m)^\s*#define\s+DOLL_DISPLAY_UPSIDE_DOWN\s+1\b", text))
    return selected, upside_down


def write_main_variant(path: Path, selected_macro: str, upside_down: bool) -> None:
    """Selects exactly one supported display macro in BoardVariant.h."""

    if selected_macro not in MAIN_VARIANTS.values():
        raise ValueError(f"Unsupported DOLL-OS board macro: {selected_macro}")
    text = path.read_text(encoding="utf-8")
    for macro in MAIN_VARIANTS.values():
        pattern = rf"(?m)^\s*(?://\s*)?#define\s+{re.escape(macro)}\s*$"
        replacement = f"#define {macro}" if macro == selected_macro else f"//#define {macro}"
        text, count = re.subn(pattern, replacement, text, count=1)
        if count != 1:
            raise ValueError(f"Could not find {macro} in {path.name}.")
    display_value = "1" if upside_down else "0"
    text, count = re.subn(
        r"(?m)^(\s*#define\s+DOLL_DISPLAY_UPSIDE_DOWN\s+)[01](\b.*)$",
        rf"\g<1>{display_value}\g<2>",
        text,
        count=1,
    )
    if count != 1:
        raise ValueError(f"Could not find DOLL_DISPLAY_UPSIDE_DOWN in {path.name}.")
    _atomic_write(path, text)


def read_numeric_defines(path: Path, names: tuple[str, ...]) -> dict[str, int]:
    """Reads named integer preprocessor defines from a hardware options header."""

    if not path.is_file():
        return {}
    text = path.read_text(encoding="utf-8")
    values: dict[str, int] = {}
    for name in names:
        match = re.search(rf"(?m)^\s*#define\s+{re.escape(name)}\s+(-?\d+)\b", text)
        if match:
            values[name] = int(match.group(1))
    return values


def write_numeric_defines(path: Path, values: Mapping[str, int]) -> None:
    """Updates named integer preprocessor defines while preserving comments."""

    text = path.read_text(encoding="utf-8")
    for name, value in values.items():
        pattern = rf"(?m)^(\s*#define\s+{re.escape(name)}\s+)-?\d+(\b.*)$"
        text, count = re.subn(pattern, rf"\g<1>{int(value)}\g<2>", text, count=1)
        if count != 1:
            raise ValueError(f"Could not find {name} in {path.name}.")
    _atomic_write(path, text)


def build_commands(
    paths: ProjectPaths,
    target: FirmwareTarget,
    action: FlashAction,
    cli: str,
    port: str = "",
    erase_flash: bool = False,
) -> list[list[str]]:
    """Builds the Arduino CLI command queue used by the non-blocking GUI runner."""

    if not cli:
        raise ValueError("Arduino CLI has not been selected.")
    sketch = paths.main_sketch if target is FirmwareTarget.MAIN else paths.slave_sketch
    if not sketch.is_dir():
        raise FileNotFoundError(f"Firmware project was not found: {sketch}")
    profile = "esp32s3"
    build_path = paths.build_root / target.value
    compile_command = [
        cli,
        "compile",
        "--profile",
        profile,
        "--build-path",
        str(build_path),
        str(sketch),
    ]
    if action is FlashAction.BUILD:
        return [compile_command]
    if not port.strip():
        raise ValueError("Select a serial port before flashing.")
    upload_command = [
        cli,
        "upload",
        "--profile",
        profile,
        "--build-path",
        str(build_path),
        "--port",
        port.strip(),
    ]
    if erase_flash:
        upload_command.extend(("--board-options", "EraseFlash=all"))
    upload_command.append(str(sketch))
    return [compile_command, upload_command]


def display_command(command: list[str]) -> str:
    """Formats a subprocess command for the log without invoking a shell."""

    return " ".join(f'"{part}"' if any(character.isspace() for character in part) else part for part in command)


def platform_summary() -> str:
    """Returns a concise runtime label for diagnostics in the application log."""

    return f"Python {sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro} on {sys.platform}"
