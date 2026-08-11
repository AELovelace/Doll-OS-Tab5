//   Doll-OS-Tab5.ino
// Entry point for the Tab5-exclusive DOLL-OS fork. The onboard display mirrors
// the shell while the official keyboard and telnet feed the shared editor.
// Bluetooth and USB keyboards will join that same input hub after hardware test.
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "BoardPins.h"
#include "config.h"
#include "global.h"

void setup() {
    Serial.begin(115200);

    unsigned long serialStart = millis();
    while (!Serial && millis() - serialStart < 3000) {
        delay(10);
    }

    Serial.println();
    Serial.printf("Starting DOLL-OS on %s...\n", DOLL_BOARD_NAME);
    Serial.flush();   //force this out over UART now, in case something below hangs before the next line

    auto m5Config = M5.config();
    m5Config.serial_baudrate = 0;                 // Serial is already initialized above.
    m5Config.clear_display = false;               // Display.ino paints the first complete frame.
    m5Config.output_power = true;                 // Keep Ext.Port1 and USB-A power available.
    m5Config.internal_imu = false;                // Defer unused devices to later port milestones.
    m5Config.internal_rtc = false;
    m5Config.internal_mic = false;
    m5Config.internal_spk = false;
    m5Config.external_imu = false;
    m5Config.external_rtc = false;
    M5.begin(m5Config);                           // Detects every supported Tab5 display revision.
    M5.Power.setExtOutput(true);                  // Enables external keyboard and USB host power.

    ledBegin();

    //report PSRAM up front -- if it is not enabled, the ~1.8MB frame sprite and the
    //history ring stay in internal SRAM and everything below is starved for it
    reportPsramStatus();

    //flip the general heap over to PSRAM before anything big allocates, so the display,
    //WiFi, storage and every later malloc/new/String below spill into PSRAM instead of
    //internal SRAM wherever they can (see enablePsramHeap)
    enablePsramHeap();

    //bring the panel up first so boot progress is visible on it too -- it mirrors
    //the shell session (see Display.ino) but does not gate on a network client
    Serial.println("[boot] initDisplay()...");
    Serial.flush();
    initDisplay();
    drawDisplayBootSplash();
    Serial.println("[boot] display OK");
    Serial.flush();

    recordHeapCheckpoint("setup start");
    reserveHotStrings();
    recordHeapCheckpoint("after reserve");

    //STA only. DOLL-OS used to run an always-on softAP alongside STA as a fallback
    //telnet path, but AP+STA on the S3's single radio cost too much streaming
    //throughput (Radio.ino audio starved once its buffer drained) and the AP
    //went unused -- the panel + official keyboard cover the no-network case.
    Serial.println("[boot] WiFi STA...");
    Serial.flush();
    WiFi.setPins(WIFI_SDIO_CLK_PIN, WIFI_SDIO_CMD_PIN,
                 WIFI_SDIO_D0_PIN, WIFI_SDIO_D1_PIN,
                 WIFI_SDIO_D2_PIN, WIFI_SDIO_D3_PIN,
                 WIFI_SDIO_RESET_PIN);
    WiFi.mode(WIFI_STA);
    connectToInternet();
    recordHeapCheckpoint("after wifi");
    Serial.println("[boot] WiFi OK");
    Serial.flush();

    Serial.println("[boot] initStorage()...");
    Serial.flush();
    initStorage();
    seedBundledApps();
    recordHeapCheckpoint("after storage");
    Serial.println("[boot] storage OK");
    Serial.flush();

    Serial.println("[boot] init Tab5 Keyboard...");
    Serial.flush();
    initKeyboardSerial();

    slaveLinkBegin();                              // Compatibility no-op on the Tab5-only fork.

    telnetServer.begin();
    telnetServer.setNoDelay(true);

    Serial.println();
    Serial.println("Telnet server started.");
    if (wifiIsConnected() == 1) {
        Serial.printf("  Station IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("  (WiFi not connected yet -- telnet reachable once STA joins)");
    }
    Serial.println("Connect with: telnet <ip> 23");

    //start the interactive shell right now instead of waiting for a telnet client to
    //connect -- the panel + Tab5 keyboard (KeyboardSerial.ino) are a complete UI on their
    //own, so present the welcome banner and prompt immediately. A telnet client that
    //dials in later re-runs this same sequence for its own screen (acceptTelnetClient()).
    beginShellSession();
    if (wifiIsConnected() == 1) {
        outLine("DOLL-OS ready. Station IP: " + WiFi.localIP().toString());
        outLine("Connect with: telnet " + WiFi.localIP().toString() + " 23");
    } else {
        outLine("DOLL-OS ready. WiFi not connected -- run 'wifi connect' for telnet access.");
    }
    outLine("");
    setActiveInput(shellPrompt(), "", false);   //panel's command bar starts on the same path-aware
                                                 //prompt the telnet side gets from printPrompt()
    printPrompt();
    drawDisplayFrame();
}

void loop() {
    acceptTelnetClient();
    readTelnetClient();
    readKeyboardSerial();   //official Tab5 Keyboard events normalized into terminal bytes
    ftpService();           //drives the FTP server one non-blocking step when active (FtpServer.ino)
    radioService();         //prints whatever the radio task/callbacks stashed (Radio.ino)
    maintainInternetConnection();
    ledService();
    drawDisplayFrame();   //mirrors whatever changed this tick -- history, status bar, live input line
    delay(1);
}
