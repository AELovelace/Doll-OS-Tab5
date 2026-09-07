"""Unit tests for Sakura Flasher's source-safe configuration helpers."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from sakura_flasher.core import (
    MAIN_VARIANTS,
    FirmwareTarget,
    FlashAction,
    ProjectPaths,
    build_commands,
    read_cpp_strings,
    read_main_variant,
    read_numeric_defines,
    replace_cpp_string,
    write_main_variant,
    write_numeric_defines,
)


class ConfigurationTests(unittest.TestCase):
    """Checks that GUI-managed C++ headers remain valid and narrowly edited."""

    def test_cpp_string_round_trip_handles_quotes_and_slashes(self) -> None:
        source = 'const char* FTP_PASS = "old";\n'
        updated = replace_cpp_string(source, "FTP_PASS", 'petal\\path"secret')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config.h"
            path.write_text(updated, encoding="utf-8")
            self.assertEqual(read_cpp_strings(path, ("FTP_PASS",))["FTP_PASS"], 'petal\\path"secret')

    def test_main_variant_writer_selects_exactly_one_macro(self) -> None:
        macros = list(MAIN_VARIANTS.values())
        source = "#define DOLL_DISPLAY_UPSIDE_DOWN 0  // rotation\n" + "\n".join(
            f"{'#define' if index == 0 else '//#define'} {macro}" for index, macro in enumerate(macros)
        )
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "BoardVariant.h"
            path.write_text(source + "\n", encoding="utf-8")
            write_main_variant(path, macros[2], True)
            selected, upside_down = read_main_variant(path)
            self.assertEqual(selected, macros[2])
            self.assertTrue(upside_down)
            self.assertEqual(path.read_text(encoding="utf-8").count("\n#define FNK"), 1)

    def test_numeric_define_writer_preserves_inline_comments(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "BoardVariant.h"
            path.write_text("#define SLAVE_ROTARY_REVERSED 0 // keep me\n", encoding="utf-8")
            write_numeric_defines(path, {"SLAVE_ROTARY_REVERSED": 1})
            self.assertEqual(read_numeric_defines(path, ("SLAVE_ROTARY_REVERSED",))["SLAVE_ROTARY_REVERSED"], 1)
            self.assertIn("// keep me", path.read_text(encoding="utf-8"))


class CommandTests(unittest.TestCase):
    """Checks that compile/upload queues remain deterministic and shell-free."""

    def make_paths(self, root: Path) -> ProjectPaths:
        main = root / "firmware" / "DS"
        slave = root / "DS-Slave"
        main.mkdir(parents=True)
        slave.mkdir()
        return ProjectPaths(root, main, slave, root / ".arduino-build")

    def test_main_flash_uses_profile_build_path_port_and_erase(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            paths = self.make_paths(Path(folder))
            commands = build_commands(paths, FirmwareTarget.MAIN, FlashAction.FLASH, "arduino-cli", "COM7", True)
            self.assertEqual(len(commands), 2)
            self.assertIn("compile", commands[0])
            self.assertIn("upload", commands[1])
            self.assertIn("COM7", commands[1])
            self.assertIn("EraseFlash=all", commands[1])
            self.assertIn("--profile", commands[0])

    def test_build_does_not_require_a_port(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            paths = self.make_paths(Path(folder))
            commands = build_commands(paths, FirmwareTarget.SLAVE, FlashAction.BUILD, "arduino-cli")
            self.assertEqual(len(commands), 1)
            self.assertNotIn("--port", commands[0])


if __name__ == "__main__":
    unittest.main()
