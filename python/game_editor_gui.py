"""Sakura-themed DOLL-OS firmware builder and flashing desktop application."""

from __future__ import annotations

import sys
from collections import deque
from pathlib import Path

from PySide6.QtCore import QProcess, QSettings, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QLinearGradient, QPainter, QPalette
from PySide6.QtSerialPort import QSerialPortInfo
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from sakura_flasher.core import (
    MAIN_STRING_FIELDS,
    MAIN_VARIANTS,
    SLAVE_BOOLEAN_FIELDS,
    FirmwareTarget,
    FlashAction,
    build_commands,
    discover_project_paths,
    display_command,
    find_arduino_cli,
    platform_summary,
    read_cpp_strings,
    read_main_variant,
    read_numeric_defines,
    write_main_config,
    write_main_variant,
    write_numeric_defines,
)


SAKURA_STYLE = """
QWidget {
    color: #4f3344;
    font-family: "Segoe UI";
    font-size: 10pt;
}
QFrame#card {
    background: rgba(255, 250, 252, 238);
    border: 1px solid #efc6d3;
    border-radius: 16px;
}
QLabel#title {
    color: #702f4e;
    font-size: 24pt;
    font-weight: 700;
}
QLabel#subtitle { color: #8e6074; font-size: 10.5pt; }
QLabel#sectionTitle { color: #7e3655; font-size: 13pt; font-weight: 650; }
QLabel#statusPill {
    background: #f7dce6;
    border: 1px solid #e8abc0;
    border-radius: 10px;
    color: #753750;
    padding: 4px 10px;
    font-weight: 600;
}
QLineEdit, QComboBox {
    background: #fffafd;
    border: 1px solid #dcaabd;
    border-radius: 8px;
    min-height: 30px;
    padding: 2px 8px;
    selection-background-color: #d86f98;
}
QLineEdit:focus, QComboBox:focus { border: 2px solid #d86f98; }
QComboBox::drop-down { border: 0; width: 24px; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 17px; height: 17px; }
QPushButton {
    background: #fff8fb;
    border: 1px solid #d79eb4;
    border-radius: 9px;
    min-height: 32px;
    padding: 3px 13px;
    font-weight: 600;
}
QPushButton:hover { background: #fbe2eb; border-color: #c87596; }
QPushButton:pressed { background: #f3c8d8; }
QPushButton:disabled { color: #ae929d; background: #f1e8eb; border-color: #dfd1d6; }
QPushButton#primaryButton { background: #d86f98; color: white; border-color: #c65d86; }
QPushButton#primaryButton:hover { background: #c95e88; }
QPushButton#dangerButton { color: #9b3f55; }
QPlainTextEdit {
    background: #382935;
    color: #f9e6ee;
    border: 1px solid #8f6073;
    border-radius: 12px;
    padding: 8px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9pt;
    selection-background-color: #b6537b;
}
QScrollArea { border: 0; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 3px; }
QScrollBar::handle:vertical { background: #dea8bb; border-radius: 4px; min-height: 24px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
"""

APP_VERSION = "0.1.0"


class SakuraCanvas(QWidget):
    """Paints a quiet blossom gradient behind the ordinary Qt widgets."""

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt owns this method name.
        painter = QPainter(self)
        gradient = QLinearGradient(0, 0, self.width(), self.height())
        gradient.setColorAt(0.0, QColor("#fff8fb"))
        gradient.setColorAt(0.55, QColor("#fbeaf0"))
        gradient.setColorAt(1.0, QColor("#f4d7e2"))
        painter.fillRect(self.rect(), gradient)
        painter.setPen(Qt.PenStyle.NoPen)
        petals = ((0.08, 0.10, 18), (0.91, 0.08, 25), (0.96, 0.32, 13), (0.05, 0.72, 22), (0.86, 0.91, 17))
        for x_ratio, y_ratio, size in petals:
            painter.setBrush(QColor(232, 153, 181, 55))
            painter.drawEllipse(int(self.width() * x_ratio), int(self.height() * y_ratio), size, int(size * 0.62))
        super().paintEvent(event)


class SakuraCard(QFrame):
    """Provides one reusable rounded panel for each configuration section."""

    def __init__(self, title: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("card")
        self.body = QVBoxLayout(self)
        self.body.setContentsMargins(18, 16, 18, 18)
        self.body.setSpacing(11)
        heading = QLabel(title)
        heading.setObjectName("sectionTitle")
        self.body.addWidget(heading)


class SakuraFlasherWindow(QMainWindow):
    """Coordinates configuration, serial discovery, and Arduino CLI processes."""

    def __init__(self) -> None:
        super().__init__()
        self.paths = discover_project_paths()
        self.settings = QSettings("DOLL-OS", "Sakura Flasher")
        self.process = QProcess(self)
        self.command_queue: deque[list[str]] = deque()
        self.active_command: list[str] | None = None
        self._build_ui()
        self._connect_process()
        self._restore_settings()
        self._load_project_configuration()
        self.refresh_ports()
        self._update_target_ui()
        self._update_cli_status()
        self._append_log("Sakura Flasher ready — " + platform_summary())

    def _build_ui(self) -> None:
        """Creates the complete first-version interface and Sakura presentation."""

        self.setWindowTitle(f"DOLL-OS Sakura Flasher {APP_VERSION}")
        self.resize(1040, 780)
        self.setMinimumSize(860, 660)
        canvas = SakuraCanvas()
        self.setCentralWidget(canvas)
        outer = QVBoxLayout(canvas)
        outer.setContentsMargins(24, 20, 24, 22)
        outer.setSpacing(14)

        header = QHBoxLayout()
        title_column = QVBoxLayout()
        title = QLabel("✿  DOLL-OS Sakura Flasher")
        title.setObjectName("title")
        subtitle = QLabel("Configure, build, and bloom fresh firmware onto your paired ESP32-S3 boards.")
        subtitle.setObjectName("subtitle")
        title_column.addWidget(title)
        title_column.addWidget(subtitle)
        header.addLayout(title_column, 1)
        self.status_pill = QLabel("Ready")
        self.status_pill.setObjectName("statusPill")
        self.status_pill.setAlignment(Qt.AlignmentFlag.AlignCenter)
        header.addWidget(self.status_pill)
        outer.addLayout(header)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        content_layout = QVBoxLayout(content)
        content_layout.setContentsMargins(1, 1, 1, 1)
        content_layout.setSpacing(14)
        scroll.setWidget(content)
        outer.addWidget(scroll, 1)

        connection_card = SakuraCard("1. Choose the blossom to build")
        connection_grid = QGridLayout()
        connection_grid.setHorizontalSpacing(10)
        connection_grid.setVerticalSpacing(9)
        self.target_combo = QComboBox()
        self.target_combo.addItem("DOLL-OS main display", FirmwareTarget.MAIN)
        self.target_combo.addItem("DS-Slave input bridge", FirmwareTarget.SLAVE)
        self.target_combo.currentIndexChanged.connect(self._update_target_ui)
        self.project_label = QLabel()
        self.project_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.project_label.setWordWrap(True)
        self.port_combo = QComboBox()
        self.port_combo.setEditable(True)
        refresh_button = QPushButton("Refresh ports")
        refresh_button.clicked.connect(self.refresh_ports)
        self.cli_edit = QLineEdit()
        self.cli_edit.setPlaceholderText("Path to arduino-cli")
        self.cli_edit.textChanged.connect(self._update_cli_status)
        browse_button = QPushButton("Browse…")
        browse_button.clicked.connect(self._browse_cli)
        connection_grid.addWidget(QLabel("Firmware"), 0, 0)
        connection_grid.addWidget(self.target_combo, 0, 1, 1, 2)
        connection_grid.addWidget(QLabel("Project"), 1, 0)
        connection_grid.addWidget(self.project_label, 1, 1, 1, 2)
        connection_grid.addWidget(QLabel("Serial port"), 2, 0)
        connection_grid.addWidget(self.port_combo, 2, 1)
        connection_grid.addWidget(refresh_button, 2, 2)
        connection_grid.addWidget(QLabel("Arduino CLI"), 3, 0)
        connection_grid.addWidget(self.cli_edit, 3, 1)
        connection_grid.addWidget(browse_button, 3, 2)
        connection_grid.setColumnStretch(1, 1)
        connection_card.body.addLayout(connection_grid)
        content_layout.addWidget(connection_card)

        options_card = SakuraCard("2. Pick the build options")
        self.options_stack = QStackedWidget()
        self.options_stack.addWidget(self._create_main_options())
        self.options_stack.addWidget(self._create_slave_options())
        options_card.body.addWidget(self.options_stack)
        content_layout.addWidget(options_card)

        action_card = SakuraCard("3. Build or flash")
        action_row = QHBoxLayout()
        self.erase_checkbox = QCheckBox("Erase all flash before upload")
        self.erase_checkbox.setToolTip("Also clears saved Wi-Fi, settings, paired devices, and internal files.")
        action_row.addWidget(self.erase_checkbox)
        action_row.addStretch(1)
        self.save_button = QPushButton("Save options")
        self.save_button.clicked.connect(self.save_configuration)
        self.build_button = QPushButton("Build")
        self.build_button.clicked.connect(lambda: self.start_action(FlashAction.BUILD))
        self.flash_button = QPushButton("Build & Flash ✿")
        self.flash_button.setObjectName("primaryButton")
        self.flash_button.clicked.connect(lambda: self.start_action(FlashAction.FLASH))
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setObjectName("dangerButton")
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.cancel_action)
        for button in (self.save_button, self.build_button, self.flash_button, self.cancel_button):
            action_row.addWidget(button)
        action_card.body.addLayout(action_row)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMinimumHeight(220)
        self.log.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        action_card.body.addWidget(self.log)
        content_layout.addWidget(action_card)

    def _create_main_options(self) -> QWidget:
        """Creates DOLL-OS panel and first-boot credentials controls."""

        panel = QWidget()
        form = QFormLayout(panel)
        form.setHorizontalSpacing(20)
        form.setVerticalSpacing(9)
        self.variant_combo = QComboBox()
        for label, macro in MAIN_VARIANTS.items():
            self.variant_combo.addItem(label, macro)
        self.upside_down_checkbox = QCheckBox("Rotate the display mounting by 180°")
        self.main_edits: dict[str, QLineEdit] = {}
        labels = {
            "STA_DEFAULT_SSID": "Default Wi-Fi name",
            "STA_DEFAULT_PASSWORD": "Default Wi-Fi password",
            "FTP_USER": "FTP username",
            "FTP_PASS": "FTP password",
            "ASUKA_BRAVE_API_KEY": "Brave Search API key",
            "ASUKA_OPENWEATHER_API_KEY": "OpenWeather API key",
        }
        form.addRow("Display board", self.variant_combo)
        form.addRow("Orientation", self.upside_down_checkbox)
        for name in MAIN_STRING_FIELDS:
            edit = QLineEdit()
            if "PASSWORD" in name or "PASS" in name or "API_KEY" in name:
                edit.setEchoMode(QLineEdit.EchoMode.Password)
                edit.setClearButtonEnabled(True)
            if "API_KEY" in name:
                edit.setPlaceholderText("Optional")
            self.main_edits[name] = edit
            form.addRow(labels[name], edit)
        note = QLabel("Secrets are written only to ignored firmware/DS/config.h and are never placed in the build log.")
        note.setObjectName("subtitle")
        note.setWordWrap(True)
        form.addRow("", note)
        return panel

    def _create_slave_options(self) -> QWidget:
        """Creates DS-Slave case-orientation and control wiring toggles."""

        panel = QWidget()
        form = QFormLayout(panel)
        form.setHorizontalSpacing(20)
        form.setVerticalSpacing(10)
        descriptions = {
            "SLAVE_ROTARY_REVERSED": "Reverse rotary direction",
            "SLAVE_JOYSTICK_INVERT_X": "Invert joystick X axis",
            "SLAVE_JOYSTICK_INVERT_Y": "Invert joystick Y axis",
            "SLAVE_SELECT_IS_FOURTH_BUTTON": "GPIO4 is a fourth Select button",
            "SLAVE_BUTTON_BAR_ACTIVE_LOW": "Button bar is active-low / common GND",
        }
        self.slave_checks: dict[str, QCheckBox] = {}
        for name in SLAVE_BOOLEAN_FIELDS:
            checkbox = QCheckBox(descriptions[name])
            self.slave_checks[name] = checkbox
            form.addRow(name.replace("SLAVE_", "").replace("_", " ").title(), checkbox)
        note = QLabel("These switches update DS-Slave/BoardVariant.h before its pinned build profile is compiled.")
        note.setObjectName("subtitle")
        note.setWordWrap(True)
        form.addRow("", note)
        return panel

    def _connect_process(self) -> None:
        """Connects QProcess signals so build output never blocks the interface."""

        self.process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.process.readyReadStandardOutput.connect(self._drain_process_output)
        self.process.finished.connect(self._process_finished)
        self.process.errorOccurred.connect(self._process_error)

    def _restore_settings(self) -> None:
        """Restores harmless UI preferences without duplicating firmware secrets."""

        preferred_cli = str(self.settings.value("arduinoCli", ""))
        self.cli_edit.setText(find_arduino_cli(self.paths.root, preferred_cli))
        target_value = str(self.settings.value("target", FirmwareTarget.MAIN.value))
        for index in range(self.target_combo.count()):
            if str(self.target_combo.itemData(index)) == target_value:
                self.target_combo.setCurrentIndex(index)
                break
        self.erase_checkbox.setChecked(False)

    def _load_project_configuration(self) -> None:
        """Loads both repositories' current options into their matching controls."""

        config_path = self.paths.main_sketch / "config.h"
        if not config_path.is_file():
            config_path = self.paths.main_sketch / "config.h.example"
        for name, value in read_cpp_strings(config_path).items():
            self.main_edits[name].setText(value)
        selected_macro, upside_down = read_main_variant(self.paths.main_sketch / "BoardVariant.h")
        variant_index = self.variant_combo.findData(selected_macro)
        if variant_index >= 0:
            self.variant_combo.setCurrentIndex(variant_index)
        self.upside_down_checkbox.setChecked(upside_down)
        slave_values = read_numeric_defines(self.paths.slave_sketch / "BoardVariant.h", SLAVE_BOOLEAN_FIELDS)
        for name, checkbox in self.slave_checks.items():
            checkbox.setChecked(bool(slave_values.get(name, 0)))

    def current_target(self) -> FirmwareTarget:
        """Returns the enum stored behind the visible firmware selector."""

        return FirmwareTarget(str(self.target_combo.currentData()))

    def _update_target_ui(self) -> None:
        """Switches build controls and project details to the selected board."""

        target = self.current_target()
        is_main = target is FirmwareTarget.MAIN
        self.options_stack.setCurrentIndex(0 if is_main else 1)
        sketch = self.paths.main_sketch if is_main else self.paths.slave_sketch
        self.project_label.setText(str(sketch))
        self.settings.setValue("target", target.value)

    def _update_cli_status(self) -> None:
        """Explains whether the selected Arduino CLI executable is ready."""

        ready = Path(self.cli_edit.text().strip()).is_file()
        if self.process.state() == QProcess.ProcessState.NotRunning:
            self.status_pill.setText("Ready" if ready else "Arduino CLI needed")
        self.build_button.setEnabled(ready and self.process.state() == QProcess.ProcessState.NotRunning)
        self.flash_button.setEnabled(ready and self.process.state() == QProcess.ProcessState.NotRunning)

    def _browse_cli(self) -> None:
        """Lets the user locate Arduino CLI when automatic discovery misses it."""

        executable, _ = QFileDialog.getOpenFileName(self, "Choose Arduino CLI", self.cli_edit.text())
        if executable:
            self.cli_edit.setText(executable)
            self.settings.setValue("arduinoCli", executable)

    def refresh_ports(self) -> None:
        """Refreshes serial devices using Qt's cross-platform port discovery."""

        previous = self.port_combo.currentData() or str(self.settings.value("port", ""))
        self.port_combo.clear()
        ports = sorted(QSerialPortInfo.availablePorts(), key=lambda info: info.portName())
        for info in ports:
            details = info.description().strip() or "Serial device"
            if info.hasVendorIdentifier() and info.hasProductIdentifier():
                details += f" · {info.vendorIdentifier():04X}:{info.productIdentifier():04X}"
            self.port_combo.addItem(f"{info.portName()} — {details}", info.portName())
        if previous:
            index = self.port_combo.findData(previous)
            if index >= 0:
                self.port_combo.setCurrentIndex(index)
            elif not ports:
                self.port_combo.setEditText(str(previous))
        if not ports:
            self.port_combo.setPlaceholderText("No serial devices found")
        self._append_log(f"Found {len(ports)} serial port{'s' if len(ports) != 1 else ''}.")

    def selected_port(self) -> str:
        """Returns the raw port name rather than its descriptive combo-box label."""

        data = self.port_combo.currentData()
        if data:
            return str(data)
        text = self.port_combo.currentText().strip()
        return text.split(" — ", 1)[0].strip()

    def save_configuration(self, show_confirmation: bool = True) -> bool:
        """Writes the currently visible board options to their source headers."""

        try:
            if self.current_target() is FirmwareTarget.MAIN:
                values = {name: edit.text() for name, edit in self.main_edits.items()}
                write_main_config(self.paths.main_sketch, values)
                write_main_variant(
                    self.paths.main_sketch / "BoardVariant.h",
                    self.variant_combo.currentData(),
                    self.upside_down_checkbox.isChecked(),
                )
                saved_path = self.paths.main_sketch / "config.h"
            else:
                values = {name: int(checkbox.isChecked()) for name, checkbox in self.slave_checks.items()}
                write_numeric_defines(self.paths.slave_sketch / "BoardVariant.h", values)
                saved_path = self.paths.slave_sketch / "BoardVariant.h"
            self._append_log(f"Saved build options to {saved_path}")
            if show_confirmation:
                self.status_pill.setText("Options saved ✿")
                QTimer.singleShot(2200, self._update_cli_status)
            return True
        except (OSError, ValueError) as error:
            self._show_error("Could not save build options", str(error))
            return False

    def start_action(self, action: FlashAction) -> None:
        """Validates settings and starts the requested non-blocking command queue."""

        if self.process.state() != QProcess.ProcessState.NotRunning:
            return
        if not self.save_configuration(show_confirmation=False):
            return
        if action is FlashAction.FLASH and self.erase_checkbox.isChecked():
            answer = QMessageBox.warning(
                self,
                "Erase all device data?",
                "This clears saved Wi-Fi, settings, paired devices, and internal files before upload. Continue?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
                QMessageBox.StandardButton.Cancel,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        port = self.selected_port()
        try:
            commands = build_commands(
                self.paths,
                self.current_target(),
                action,
                self.cli_edit.text().strip(),
                port,
                self.erase_checkbox.isChecked(),
            )
        except (OSError, ValueError) as error:
            self._show_error("Cannot start build", str(error))
            return
        self.settings.setValue("arduinoCli", self.cli_edit.text().strip())
        if port:
            self.settings.setValue("port", port)
        self.command_queue = deque(commands)
        self._set_busy(True)
        self._append_log("\n── Sakura build started ──")
        self._run_next_command()

    def _run_next_command(self) -> None:
        """Starts the next compile/upload command after the previous one succeeds."""

        if not self.command_queue:
            self.active_command = None
            self._set_busy(False)
            self.status_pill.setText("Complete ✿")
            self._append_log("── Finished successfully. Your firmware is in bloom! ✿ ──\n")
            QTimer.singleShot(3500, self._update_cli_status)
            return
        self.active_command = self.command_queue.popleft()
        self.paths.build_root.mkdir(parents=True, exist_ok=True)
        self._append_log("$ " + display_command(self.active_command))
        self.process.setWorkingDirectory(str(self.paths.root))
        self.process.start(self.active_command[0], self.active_command[1:])

    def _drain_process_output(self) -> None:
        """Moves all currently available Arduino CLI output into the live log."""

        output = bytes(self.process.readAllStandardOutput()).decode("utf-8", errors="replace")
        if output:
            self.log.moveCursor(self.log.textCursor().MoveOperation.End)
            self.log.insertPlainText(output)
            self.log.moveCursor(self.log.textCursor().MoveOperation.End)

    def _process_finished(self, exit_code: int, exit_status: QProcess.ExitStatus) -> None:
        """Advances successful queues and stops immediately after a failed command."""

        self._drain_process_output()
        if exit_status == QProcess.ExitStatus.NormalExit and exit_code == 0:
            self._run_next_command()
            return
        self.command_queue.clear()
        self._set_busy(False)
        self.status_pill.setText("Build failed")
        self._append_log(f"── Command failed with exit code {exit_code}. See the messages above. ──\n")

    def _process_error(self, error: QProcess.ProcessError) -> None:
        """Reports process-launch failures that do not provide normal CLI output."""

        if error == QProcess.ProcessError.FailedToStart:
            self.command_queue.clear()
            self._set_busy(False)
            self.status_pill.setText("Could not start")
            self._append_log(f"Process error: {self.process.errorString()}")
            return
        if error == QProcess.ProcessError.Crashed and self.process.state() == QProcess.ProcessState.NotRunning:
            return
        self._append_log(f"Process error: {self.process.errorString()}")

    def cancel_action(self) -> None:
        """Cancels queued work and asks the active Arduino CLI process to stop."""

        self.command_queue.clear()
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.process.terminate()
            if not self.process.waitForFinished(1500):
                self.process.kill()
        self._set_busy(False)
        self.status_pill.setText("Cancelled")
        self._append_log("── Build cancelled. ──\n")

    def _set_busy(self, busy: bool) -> None:
        """Locks mutating controls while a compiler or uploader owns the process."""

        self.status_pill.setText("Working…" if busy else "Ready")
        self.target_combo.setEnabled(not busy)
        self.save_button.setEnabled(not busy)
        self.build_button.setEnabled(not busy and Path(self.cli_edit.text().strip()).is_file())
        self.flash_button.setEnabled(not busy and Path(self.cli_edit.text().strip()).is_file())
        self.cancel_button.setEnabled(busy)

    def _append_log(self, message: str) -> None:
        """Appends one human-readable status line and keeps the newest text visible."""

        if not hasattr(self, "log"):
            return
        self.log.appendPlainText(message)
        self.log.verticalScrollBar().setValue(self.log.verticalScrollBar().maximum())

    def _show_error(self, title: str, detail: str) -> None:
        """Shows an actionable modal error and mirrors it into the persistent log."""

        self.status_pill.setText("Needs attention")
        self._append_log(f"Error: {detail}")
        QMessageBox.critical(self, title, detail)

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt owns this method name.
        """Prevents the window from abandoning a compiler process without consent."""

        if self.process.state() != QProcess.ProcessState.NotRunning:
            answer = QMessageBox.question(
                self,
                "Cancel the active build?",
                "A build or upload is still running. Cancel it and close Sakura Flasher?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                event.ignore()
                return
            self.cancel_action()
        event.accept()


def main() -> int:
    """Initializes Qt styling and starts the Sakura Flasher event loop."""

    app = QApplication(sys.argv)
    app.setApplicationName("DOLL-OS Sakura Flasher")
    app.setOrganizationName("DOLL-OS")
    app.setStyle("Fusion")
    app.setStyleSheet(SAKURA_STYLE)
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Highlight, QColor("#d86f98"))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor("#ffffff"))
    app.setPalette(palette)
    app.setFont(QFont("Segoe UI", 10))
    window = SakuraFlasherWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
