"""Routes the dedicated emulator firmware to its ota_1 flash partition."""

from pathlib import Path
import csv

Import("env")


def parse_hex(value: str) -> int:
    """Parses the hexadecimal offsets and sizes used by partitions.csv."""
    return int(value.strip(), 0)


def emulator_partition() -> tuple[int, int]:
    """Returns the checked offset and size of the named emulator slot."""
    project_dir = Path(env.subst("$PROJECT_DIR"))
    partition_path = project_dir / "partitions.csv"
    with partition_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == "emulator":
                if len(row) < 5 or row[1].strip() not in ("app", "0"):
                    raise RuntimeError("'emulator' must be an application partition")
                return parse_hex(row[3]), parse_hex(row[4])
    raise RuntimeError("partitions.csv has no application named 'emulator'")


emulator_offset, emulator_size = emulator_partition()
env.Replace(ESP32_APP_OFFSET=hex(emulator_offset))
env.BoardConfig().update("upload.maximum_size", emulator_size)
env["INTEGRATION_EXTRA_DATA"].update(
    {"application_offset": hex(emulator_offset)}
)  # Keeps debugger/upload metadata aligned with esptool's ota_1 address.
print(
    f"[pio] Emulator upload target: 0x{emulator_offset:x} "
    f"({emulator_size // 1024} KiB slot)"
)
