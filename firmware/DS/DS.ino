//   DS.ino
//   entry point -- telnet-interface port of DOLL-OS for the Freenove ESP32-S3
//   display board (FNK0104-series / "FNK1014B"). No physical screen or keyboard is
//   used: a single telnet client is the entire UI, replacing M5Cardputer's sprite
//   display + keyboard as DOLL-OS's whole interaction model. See docs/PORTING.md
//   for what changed and why.
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

    //why the last reset happened, and the saved core dump if it was a panic. First thing
    //after the banner because the live panic text does not survive a USB-only cable on
    //this board -- see the crash postmortem notes in SysInfo.ino. Also available later as
    //the "crash" command, since the dump is kept until "crash clear".
    reportLastCrashSerial();
    Serial.flush();

    ledBegin();

    //report PSRAM up front -- if it's not enabled here, the ~150KB frame sprite and the
    //history ring stay in internal SRAM and everything below is starved for it
    reportPsramStatus();

    //flip the general heap over to PSRAM before anything big allocates, so the display,
    //WiFi, storage and every later malloc/new/String below spill into PSRAM instead of
    //internal SRAM wherever they can (see enablePsramHeap)
    enablePsramHeap();

    //bring the panel up first so boot progress is visible on it too -- it mirrors
    //the telnet session (see Display.ino) but doesn't gate on one existing
    Serial.println("[boot] initDisplay()...");
    Serial.flush();
    initDisplay();
    drawDisplayBootSplash();
    Serial.println("[boot] display OK");
    Serial.flush();

    recordHeapCheckpoint("setup start");
    reserveHotStrings();
    recordHeapCheckpoint("after reserve");

    //Saved Wi-Fi networks live on LittleFS, so mount internal storage before the
    //connection pass. The later initStorage() call is idempotent and adds SD_MMC.
    Serial.println("[boot] internal storage...");
    Serial.flush();
    initInternalStorage();
    Serial.println("[boot] internal storage OK");
    Serial.flush();

    //STA only. DOLL-OS used to run an always-on softAP alongside STA as a fallback
    //telnet path, but AP+STA on the S3's single radio cost too much streaming
    //throughput (Radio.ino audio starved once its buffer drained) and the AP
    //went unused -- the panel + BLE keyboard already cover the no-network case.
    Serial.println("[boot] WiFi STA...");
    Serial.flush();
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

    //second UART for the DS-Slave BLE-keyboard bridge (KeyboardSerial.ino)
    Serial.println("[boot] initKeyboardSerial()...");
    Serial.flush();
    initKeyboardSerial();

    //Hardware-UART outbound command channel to DS-Slave. KeyboardSerial UART1 is
    //full-duplex, with its TX routed to the clear variant-specific pin.
    slaveLinkBegin();

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
    //connect -- the panel + BLE keyboard (KeyboardSerial.ino) are a complete UI on their
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
    readKeyboardSerial();   //keystrokes from the DS-Slave BLE-keyboard bridge (KeyboardSerial.ino)
    padButtonService();     //acts on a DS-Slave button-bar press parked by the above (PadButtons.ino)
    ftpService();           //drives the FTP server one non-blocking step when active (FtpServer.ino)
    radioService();         //prints whatever the radio task/callbacks stashed (Radio.ino)
    maintainInternetConnection();
    ledService();
    drawDisplayFrame();   //mirrors whatever changed this tick -- history, status bar, live input line
    delay(1);
}
