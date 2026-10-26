//   SlaveLink.ino
//   Outbound command channel to DS-Slave -- the companion ESP32-S3 that bridges a
//   BLE HID keyboard to a UART (see ../DS-Slave/DS-Slave.ino). DS-Slave listens for
//   newline-terminated line commands on its UART1 RX (GPIO18) at 115200 8N1:
//     HELP  STATUS  KEYBOARDS/PAIRED  FORGET  PAIR  SCAN
//     LED <0-31>  NUM 0|1  CAPS 0|1  SCROLL 0|1  OUT <hex bytes>
//     GAME 0|1  PAD [slot] AUTO|KEYBOARD|GAMEPAD  DUMP 0|1  DROP
//   DS-Slave holds up to two HID devices at once (a keyboard and a controller),
//   merging their input into one stream, so nothing here needs to care which is
//   which; "slave status" lists the slots on the slave's own console.
//
//   KeyboardSerial (UART1, KeyboardSerial.ino) is full-duplex. Its RX receives
//   keystrokes and its TX sends these commands on SLAVE_LINK_TX_PIN. BoardPins.h
//   keeps both pins clear of each variant's onboard peripherals.
//
//   The link is deliberately one-way. DS-Slave prints every command's reply to its
//   own USB serial console (Serial.print, never back onto its UART1 TX), so DOLL-OS cannot
//   read STATUS/HELP/etc. output back over this wire -- SLAVE_LINK_TX_PIN -> GPIO18 carries
//   commands out and nothing returns on it. The keystroke stream on the *other* wire
//   (Slave GPIO17 -> KEYBOARD_SERIAL_RX_PIN) is unrelated and keeps flowing. handleSlaveCommand()
//   says as much so a user isn't left waiting for a reply that only lands on the
//   slave's console.

static const uint32_t SLAVE_LINK_BAUD = 115200;   //must match DS-Slave's LinkSerial (UART_BAUD)

static bool slaveLinkReady = false;

void slaveLinkBegin() {
    //initKeyboardSerial() has already attached UART1 RX and TX to the selected pins.
    slaveLinkReady = true;

    Serial.printf("[boot] slave link hardware UART TX=%d baud=%lu\n",
                  SLAVE_LINK_TX_PIN, (unsigned long)SLAVE_LINK_BAUD);
}

//sends one command line to DS-Slave, terminated with '\n' (its pumpCommandStream treats
//either CR or LF as end-of-line). UART1's hardware peripheral owns the bit timing.
void slaveLinkSendLine(const String& line) {
    if (!slaveLinkReady) {
        return;
    }
    ledPulseInput();
    KeyboardSerial.write((const uint8_t*)line.c_str(), line.length());
    KeyboardSerial.write('\n');
}

//   Shell command: "slave <subcommand> [args]" -- one DOLL-OS-side verb that reaches every
//   DS-Slave command over the bit-banged wire. Subcommand names mirror DS-Slave's own
//   (case-insensitive here; forwarded upper-cased where the slave expects it). "raw"
//   is an escape hatch that forwards the rest of the line verbatim.
static void slaveLinkUsage() {
    outLine("slave <subcommand> -- drive the DOLL-OS BLE-keyboard bridge", C_CYAN);
    outLine("  slave status              request STATUS");
    outLine("  slave keyboards|paired    list saved keyboards");
    outLine("  slave pair                pair one more device (existing ones stay)");
    outLine("  slave scan                rescan / reconnect");
    outLine("  slave forget              clear saved keyboards + bonds");
    outLine("  slave led <0-31>          set the keyboard LED mask");
    outLine("  slave num|caps|scroll 0|1 toggle one lock LED");
    outLine("  slave game 0|1            gamepad mode (button events vs keystrokes)");
    outLine("  slave pad [slot] auto|keyboard|gamepad  how to parse a peer's reports");
    outLine("  slave drop                disconnect every paired device (bonds kept)");
    outLine("  slave dump 0|1            hex-log raw input reports on the slave console");
    outLine("  slave out <hex bytes>     send a raw HID output report");
    outLine("  slave help                ask the slave to print its help");
    outLine("  slave raw <text...>       forward a line verbatim");
    outLine("Link is send-only: replies print on the keyboard bridge's USB console, not here.", C_YELLOW);
}

//joins parts[from..partCount) back into a space-separated string (undoing the tokenizer)
static String slaveLinkJoinFrom(const String parts[], int partCount, int from) {
    String out = "";
    for (int i = from; i < partCount; i++) {
        if (out.length() > 0) {
            out += " ";
        }
        out += parts[i];
    }
    return out;
}

void handleSlaveCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        slaveLinkUsage();
        return;
    }

    String sub = parts[1];
    sub.toLowerCase();
    String line = "";

    if (sub == "status" || sub == "help" || sub == "pair" || sub == "scan" || sub == "forget") {
        line = parts[1];
        line.toUpperCase();
    } else if (sub == "keyboards" || sub == "paired") {
        line = parts[1];
        line.toUpperCase();
    } else if (sub == "led") {
        if (partCount < 3) {
            outLine("usage: slave led <0-31>", C_RED);
            return;
        }
        line = "LED " + parts[2];
    } else if (sub == "num" || sub == "caps" || sub == "scroll") {
        if (partCount < 3) {
            outLine("usage: slave " + sub + " 0|1", C_RED);
            return;
        }
        String verb = parts[1];
        verb.toUpperCase();
        line = verb + " " + parts[2];
    } else if (sub == "game") {
        if (partCount < 3) {
            outLine("usage: slave game 0|1", C_RED);
            return;
        }
        line = "GAME " + parts[2];
    } else if (sub == "pad") {
        if (partCount < 3) {
            outLine("usage: slave pad [slot] auto|keyboard|gamepad", C_RED);
            return;
        }
        String rest = slaveLinkJoinFrom(parts, partCount, 2);   //optional slot, then the mode
        rest.toUpperCase();
        line = "PAD " + rest;
    } else if (sub == "drop") {
        line = "DROP";
    } else if (sub == "dump") {
        if (partCount < 3) {
            outLine("usage: slave dump 0|1", C_RED);
            return;
        }
        line = "DUMP " + parts[2];
    } else if (sub == "out") {
        if (partCount < 3) {
            outLine("usage: slave out <hex bytes>", C_RED);
            return;
        }
        line = "OUT " + slaveLinkJoinFrom(parts, partCount, 2);
    } else if (sub == "raw") {
        if (partCount < 3) {
            outLine("usage: slave raw <text...>", C_RED);
            return;
        }
        line = slaveLinkJoinFrom(parts, partCount, 2);   //forwarded exactly as typed
    } else {
        outLine("slave: unknown subcommand '" + parts[1] + "'", C_RED);
        slaveLinkUsage();
        return;
    }

    slaveLinkSendLine(line);
    outLine("-> DOLL-OS keyboard bridge: " + line, C_GREEN);
}
