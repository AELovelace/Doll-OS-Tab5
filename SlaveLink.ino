// SlaveLink.ino
// Tab5 has no DS-Slave companion. These compatibility functions keep shared
// command and Game Boy call sites linkable while BLE moves to the onboard C6.

void slaveLinkBegin() {
    Serial.println("[boot] DS-Slave link omitted on Tab5");
}  // Records that the Tab5-exclusive build owns its keyboards locally.

void slaveLinkSendLine(const String& line) {
    (void)line;
}  // Ignores legacy game-mode commands until the native HID game mapping lands.

void handleSlaveCommand(const String parts[], int partCount) {
    (void)parts;
    (void)partCount;
    outLine("slave: unavailable on Tab5; use native keyboard management", C_YELLOW);
}  // Replaces the obsolete bridge command with an actionable compatibility message.
