from pathlib import Path
import os
import shutil
import subprocess

Import("env")


def find_arduino_cli() -> Path:
    """Finds the Arduino CLI used only for .ino prototype generation."""
    configured = os.environ.get("DOLL_OS_ARDUINO_CLI")
    if configured and Path(configured).is_file():
        return Path(configured)

    discovered = shutil.which("arduino-cli")
    if discovered:
        return Path(discovered)

    local_app_data = Path(os.environ.get("LOCALAPPDATA", ""))
    bundled = local_app_data / "Programs" / "Arduino IDE" / "resources" / "app" / "lib" / "backend" / "resources" / "arduino-cli.exe"
    if bundled.is_file():
        return bundled

    raise RuntimeError("Arduino CLI was not found; set DOLL_OS_ARDUINO_CLI to its executable path")


def generate_sketch_source() -> None:
    """Runs Arduino's preprocessor, then exposes its generated C++ to ESP-IDF."""
    project_dir = Path(env.subst("$PROJECT_DIR"))
    generated_dir = project_dir / ".pio-generated"
    preprocess_dir = project_dir / ".pio-preprocess"
    generated_source = generated_dir / "Doll-OS-Tab5.ino.cpp"
    sketch_sources = list(project_dir.glob("*.ino"))

    newest_input = max(path.stat().st_mtime for path in sketch_sources)
    if generated_source.is_file() and generated_source.stat().st_mtime >= newest_input:
        print("[pio] Arduino sketch source is current")
        return

    generated_dir.mkdir(parents=True, exist_ok=True)
    preprocess_dir.mkdir(parents=True, exist_ok=True)
    command = [
        str(find_arduino_cli()),
        "compile",
        "--profile",
        "tab5",
        "--build-path",
        str(preprocess_dir),
        "--only-compilation-database",
        str(project_dir),
    ]
    print("[pio] Generating Arduino sketch prototypes")
    subprocess.run(command, cwd=project_dir, check=True)

    preprocessed_source = preprocess_dir / "sketch" / "Doll-OS-Tab5.ino.cpp"
    if not preprocessed_source.is_file():
        raise RuntimeError(f"Arduino preprocessor did not create {preprocessed_source}")
    shutil.copy2(preprocessed_source, generated_source)
    print(f"[pio] Generated {generated_source.relative_to(project_dir)}")


generate_sketch_source()

