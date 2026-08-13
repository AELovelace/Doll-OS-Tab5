//   Doll-OS-Tab5.ino
// Entry point for the Tab5-exclusive DOLL-OS fork. The onboard display mirrors
// the shell while the official keyboard and telnet feed the shared editor.
// Bluetooth and USB keyboards will join that same input hub after hardware test.
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include <hal/axi_icm_ll.h>
#endif
#include "BoardPins.h"
#include "config.h"
#include "global.h"

//   The P4 DSI driver continuously moves its PSRAM framebuffer through DW-GDMA. An
//   underrun is unrecoverable in the IDF driver and leaves the ST7123 solid blue, so
//   scanout needs the highest AXI read priority.
//
//   The earlier workaround lowered DSI from 15 to 8 because priority 15 by itself
//   starved ESP-Hosted's SDIO traffic until it timed out and restarted the P4. That
//   traded the reboot for the blue-screen underrun. Espressif exposes the missing half
//   of the fix: SDMMC belongs to AXI_ICM_MASTER_CPU, whose priority can be raised
//   independently. Keep scanout at 15 and the CPU/SDMMC path immediately behind it at
//   14, instead of forcing both devices to fight at the default CPU priority.
static const uint32_t DSI_SCANOUT_AXI_READ_QOS = 15;
static const uint32_t CPU_SDIO_AXI_QOS = 14;

static void prioritizeDsiScanout() {
#if CONFIG_IDF_TARGET_ESP32P4
    //The ESP32-P4 DSI bridge reads its continuous framebuffer through one of the two
    //DW-GDMA AXI master ports. Writes stay at 0: the bridge only reads the framebuffer.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, DSI_SCANOUT_AXI_READ_QOS);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, DSI_SCANOUT_AXI_READ_QOS);
    //The CPU master aggregates both CPUs, SDMMC, USB and EMAC on ESP32-P4. Raising
    //both directions keeps ESP-Hosted command/data traffic moving under scanout load.
    axi_icm_ll_set_cpu_qos_arbiter_prio(CPU_SDIO_AXI_QOS, CPU_SDIO_AXI_QOS);
    Serial.printf("[display] AXI QoS: DSI read %u/%u, CPU/SDIO read+write %u\n",
                  (unsigned)DSI_SCANOUT_AXI_READ_QOS,
                  (unsigned)DSI_SCANOUT_AXI_READ_QOS,
                  (unsigned)CPU_SDIO_AXI_QOS);
#endif
}  // Gives scanout first service and the hosted-radio transport the next priority.

void setup() {
    Serial.begin(115200);

    unsigned long serialStart = millis();
    while (!Serial && millis() - serialStart < 3000) {
        delay(10);
    }

    const bool gbaBootMode = gbaClaimBootMode();  // Claims a one-shot RTC launch before OS allocation begins.
    Serial.println();
    Serial.printf("Starting %s on %s...\n",
                  gbaBootMode ? "GBA minimal mode" : "DOLL-OS",
                  DOLL_BOARD_NAME);
    Serial.flush();   //force this out over UART now, in case something below hangs before the next line

    auto m5Config = M5.config();
    m5Config.serial_baudrate = 0;                 // Serial is already initialized above.
    // M5Unified maps clear_display=false to init_without_reset(). That shortcut is
    // unsafe after esp_restart(): the P4 resets while the ST7123 remains in its old
    // DSI state, leaving both minimal GBA mode and Doll-OS alive behind a dark panel.
    m5Config.clear_display = true;                // Fully resets/wakes the panel on every boot.
    m5Config.output_power = true;                 // Keep Ext.Port1 and USB-A power available.
    m5Config.internal_imu = false;                // Defer unused devices to later port milestones.
    m5Config.internal_rtc = false;
    m5Config.internal_mic = false;
    m5Config.internal_spk = false;
    m5Config.external_imu = false;
    m5Config.external_rtc = false;
    M5.begin(m5Config);                           // Detects every supported Tab5 display revision.
    prioritizeDsiScanout();                       // Prevents PSRAM arbitration from starving DSI scanout.
    M5.Power.setExtOutput(true);                  // Enables external keyboard and USB host power.

    //Claim the internal-I2C mutex before anything can contend for the bus (global.h).
    //Everything above this point is still single-threaded, so it needs no guard.
    boardI2cBegin();

    ledBegin();

    //report PSRAM up front -- if it is not enabled, the ~1.8MB frame sprite and the
    //history ring stay in internal SRAM and everything below is starved for it
    reportPsramStatus();

    //flip the general heap over to PSRAM before anything big allocates, so the display,
    //WiFi, storage and every later malloc/new/String below spill into PSRAM instead of
    //internal SRAM wherever they can (see enablePsramHeap)
    enablePsramHeap();

    if (gbaBootMode) {
        // Game mode deliberately stops here: no frameSprite/display shadow, shell
        // history, LittleFS settings, Wi-Fi/C6, telnet, FTP, or command runtime.
        gbaInitMinimalDisplay();                  // Paints directly into the DSI framebuffer.
        if (!initSdStorageOnly()) {
            gbaAbortBootMode("SD card unavailable");
            return;
        }
        initKeyboardSerial();                    // Keeps the official keyboard/game mappings.
        slaveLinkBegin();                        // Leaves the shared input compatibility shim ready.
        gbaRunBootMode();                        // Runs until quit, then saves and restarts into Doll-OS.
        return;
    }

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

    //Mount settings before Wi-Fi so connectToInternet() can actually read wifi.cfg.
    //The old order always queried an unmounted LittleFS volume, silently discarded
    //saved credentials, and repeatedly powered the C6 radio for YOUR_WIFI_SSID.
    Serial.println("[boot] initStorage()...");
    Serial.flush();
    initStorage();
    seedBundledApps();
    recordHeapCheckpoint("after storage");
    Serial.println("[boot] storage OK");
    Serial.flush();

    //STA only. WiFiManager initializes ESP-Hosted lazily after it has found real
    //credentials; placeholder defaults now leave the power-hungry radio switched off.
    Serial.println("[boot] WiFi STA...");
    Serial.flush();
    connectToInternet();
    recordHeapCheckpoint("after wifi");
    Serial.println("[boot] WiFi ready");
    Serial.flush();

    Serial.println("[boot] init Tab5 Keyboard...");
    Serial.flush();
    initKeyboardSerial();

    slaveLinkBegin();                              // Compatibility no-op on the Tab5-only fork.

    Serial.println();
    if (startTelnetServer() && wifiIsConnected() == 1) {
        Serial.println("Telnet server started.");
        Serial.printf("  Station IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("Telnet dormant (starts after WiFi is initialized).");
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
    recordHeapCheckpoint("setup ready");
}

void loop() {
    acceptTelnetClient();
    readTelnetClient();
    readKeyboardSerial();   //official Tab5 Keyboard events normalized into terminal bytes
    gbServiceMainTouch();   //status-bar GB launcher; opens the same picker as the `gb` command
    ftpService();           //drives the FTP server one non-blocking step when active (FtpServer.ino)
    radioService();         //prints whatever the radio task/callbacks stashed (Radio.ino)
    maintainInternetConnection();
    ledService();
    drawDisplayFrame();   //mirrors whatever changed this tick -- history, status bar, live input line
    delay(1);
}
