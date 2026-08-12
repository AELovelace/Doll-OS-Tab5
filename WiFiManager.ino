//   WiFiManager.ino
//   Wi-Fi connectivity: DOLL-OS's wifi.ino command subsystem (scan/connect/save/
//   status) driving a plain STA connection. DOLL-OS originally also ran an always-on
//   softAP here as a fallback telnet path, but AP+STA on the S3's single radio
//   cost too much streaming throughput (Radio.ino audio starved once its buffer
//   drained) and the AP went unused -- the panel + BLE keyboard already cover
//   the no-network case, so STA is now the only mode.
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
//   Co-processor OTA. The Tab5's Wi-Fi lives on an ESP32-C6 reached over SDIO
//   (ESP-Hosted), and its firmware is normally flashed through UART pads on the
//   back of the board. This header exposes the alternative: the host pushes a new
//   slave image over the SDIO link it is already using, so no programmer is needed.
//
//   Wrapped in extern "C" deliberately -- unlike most IDF headers this one carries
//   no __cplusplus guard of its own, so including it from a .ino mangles the names
//   and every call fails to link.
extern "C" {
#include "esp_hosted_ota.h"
}

const char* WIFI_CREDS_PATH = "/wifi.cfg";
static bool wifiStationReady = false;
static bool wifiReconnectEnabled = false;

static bool wifiSsidIsConfigured(const String& ssid) {
    return ssid.length() > 0 && ssid != "YOUR_WIFI_SSID";
}  // Rejects the shipped sentinel before it can start the hosted radio.

static void ensureWifiStationReady() {
    if (wifiStationReady) return;

    WiFi.setPins(WIFI_SDIO_CLK_PIN, WIFI_SDIO_CMD_PIN,
                 WIFI_SDIO_D0_PIN, WIFI_SDIO_D1_PIN,
                 WIFI_SDIO_D2_PIN, WIFI_SDIO_D3_PIN,
                 WIFI_SDIO_RESET_PIN);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    wifiStationReady = true;
}  // Starts ESP-Hosted only when boot or an explicit Wi-Fi command needs it.

bool wifiStationIsReady() {
    return wifiStationReady;
}  // Lets socket and sleep code avoid touching an uninitialized network stack.

//tries saved credentials first, falling back to the config.h defaults
//returns true if the router connection succeeded
bool connectToInternet() {
    String ssid, password;
    if (!loadWifiCredentials(ssid, password)) {
        ssid = STA_DEFAULT_SSID;
        password = STA_DEFAULT_PASSWORD;
    }

    if (!wifiSsidIsConfigured(ssid)) {
        wifiReconnectEnabled = false;
        Serial.println("WiFi skipped: no saved credentials (use 'wifi connect').");
        ledSetWifiConnected(false);
        return false;
    }

    ensureWifiStationReady();

    //Turn OFF the ESP32 core's built-in auto-reconnect (defaults to ON). Left on, a
    //failed join -- e.g. the config.h default SSID isn't present -- makes the driver
    //spin on association *forever* in the background, and that permanently-busy radio
    //blocks everything else: esp_wifi_scan_start() and esp_wifi_set_config() both fail
    //immediately while the STA is mid-connect, so "wifi scan" fails instantly and
    //"wifi connect" hits "cannot set config". DOLL-OS drives its own reconnection instead
    //(maintainInternetConnection, a bounded 10s tick), which keeps the radio idle
    //between attempts so scans/manual connects can get in. Only needs setting once,
    //but it's cheap and idempotent here.
    WiFi.setAutoReconnect(false);

    Serial.printf("Connecting to router: %s\n", ssid.c_str());
    ledPulseNetwork();
    ledSetWifiConnected(false);
    wifiReconnectEnabled = true;
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        Serial.print(".");
        delay(500);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        ledSetWifiConnected(true);
        Serial.println("Connected to router.");
        Serial.print("Station IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }

    Serial.println("Could not connect to router.");
    ledSetWifiConnected(false);
    return false;
}

unsigned long previousReconnectAttempt = 0;
const unsigned long reconnectInterval = 10000;

void maintainInternetConnection() {
    if (!wifiStationReady || !wifiReconnectEnabled) return;

    if (WiFi.status() == WL_CONNECTED) {
        ledSetWifiConnected(true);
        startTelnetServer();
        return;
    }
    ledSetWifiConnected(false);
    if (millis() - previousReconnectAttempt < reconnectInterval) {
        return;
    }
    previousReconnectAttempt = millis();
    ledPulseNetwork();

    //Re-associate using the config the last begin() already installed -- do NOT call
    //WiFi.begin() again here. begin() re-runs esp_wifi_set_config(), which the driver
    //rejects with "sta is connecting, cannot set config" whenever a prior attempt is
    //still in flight -- and a join that hasn't succeeded leaves us in exactly that
    //state, so the old disconnect()+begin() pair just logged that error on every
    //10s tick and never made progress. reconnect() re-issues the association without
    //touching set_config, so it's always accepted. (To point at a *different*
    //network, use connectWifiNetwork(), which does a clean stop before begin().)
    WiFi.reconnect();
}

int wifiIsConnected() {
    return wifiStationReady && WiFi.status() == WL_CONNECTED ? 1 : 0;
}

void scanWifiNetworks() {
    ensureWifiStationReady();
    WiFi.scanDelete();
    outLine("Scanning for Wifi Networks");
    telnetClient.flush();   //push this line out before the blocking scan begins
    ledPulseNetwork();

    //A scan can't start while the STA is mid-association: esp_wifi_scan_start() bails
    //out and scanNetworks() fails *immediately* (WIFI_SCAN_FAILED) instead of taking
    //its usual ~2s. If we're not actually connected, abort any pending association so
    //the radio is free to scan. (A live connection scans fine -- leave it alone.) The
    //scan below is synchronous, so maintainInternetConnection() can't re-associate
    //underneath us until it returns.
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        delay(100);
    }

    int networkCount = WiFi.scanNetworks();
    if (networkCount < 0) {
        outLine("Scan Failed", C_RED);
        WiFi.scanDelete();
        return;
    }
    if (networkCount == 0) {
        outLine("No Networks Found");
        WiFi.scanDelete();
        return;
    }
    outLine(String(networkCount) + " Networks Found");

    for (int i = 0; i < networkCount; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            ssid = "<hidden>";
        }
        if (ssid.length() > 20) {
            ssid = ssid.substring(0, 20) + "...";
        }
        String security = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "SECURE";
        String result = String(i + 1) + ". " + ssid + " "
                        + String(WiFi.RSSI(i)) + "dBm "
                        + "ch" + String(WiFi.channel(i)) + " " + security;
        outLine(result);
    }
    WiFi.scanDelete();
}

void wifiStatus() {
    if (wifiIsConnected() == 1) {
        outLine("WiFi connected", C_GREEN);
        outLine("SSID: " + WiFi.SSID());
        outLine("IP: " + WiFi.localIP().toString());
        outLine("Gateway: " + WiFi.gatewayIP().toString());
        outLine("Subnet: " + WiFi.subnetMask().toString());
    } else {
        outLine("WiFi Not Connected", C_RED);
    }
}

void showWifiStatus() {
    if (wifiIsConnected() == 0) {
        outLine("WiFi: not connected", C_RED);
        return;
    }
    wifiStatus();
}

void connectWifiNetwork(const String& ssid, const String& password) {
    if (!wifiSsidIsConfigured(ssid)) {
        outLine("WiFi connect needs a real SSID", C_RED);
        return;
    }

    ensureWifiStationReady();
    outLine("Connecting to: " + ssid);
    telnetClient.flush();
    ledPulseNetwork();
    ledSetWifiConnected(false);

    //Abort any association still in flight before begin(): a stuck/failed join leaves
    //the STA "connecting", and esp_wifi_set_config() (inside begin()) is rejected in
    //that state ("cannot set config"), so a user-issued network change would silently
    //fail. disconnect() (radio stays started) clears the pending attempt; the short
    //settle lets the async stop land before set_config runs. With driver auto-reconnect
    //off (connectToInternet), nothing re-associates underneath us between the two calls.
    WiFi.disconnect();
    delay(200);
    wifiReconnectEnabled = true;
    WiFi.begin(ssid.c_str(), password.c_str());

    const unsigned long timeoutMs = 15000;
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeoutMs) {
        delay(250);
    }

    if (wifiIsConnected() == 1) {
        ledSetWifiConnected(true);
        startTelnetServer();
        wifiStatus();
    } else {
        ledSetWifiConnected(false);
        ledPulseError();
        outLine("WiFi connect failed", C_RED);
    }
}

bool saveWifiCredentials(const String& ssid, const String& password) {
    ledPulseStorageWrite(false);
    File file = LittleFS.open(WIFI_CREDS_PATH, "w");
    if (!file) {
        return false;
    }
    file.println(ssid);
    file.println(password);
    file.close();
    return true;
}

bool loadWifiCredentials(String& ssid, String& password) {
    ledPulseStorageRead(false);
    File file = LittleFS.open(WIFI_CREDS_PATH, "r");
    if (!file) {
        return false;
    }
    ssid = file.readStringUntil('\n');
    password = file.readStringUntil('\n');
    file.close();

    ssid.trim();
    password.trim();
    return ssid.length() > 0;
}

void wifiHelp() {
    outLine("WiFi subcommands:");
    outLine("wifi");
    outLine("wifi scan");
    outLine("wifi connect <ssid> <password>");
    outLine("wifi save <ssid> <password>");
    outLine("wifi coproc-ota <url>");
}

//   Updates the ESP32-C6 Wi-Fi co-processor over the existing SDIO link.
//
//   Why this exists: the co-processor reports its ESP-Hosted version at every boot,
//   and a mismatch against the host stack is not cosmetic -- the transport itself
//   warns it causes RPC timeouts. On this board an outdated slave could not sustain
//   the throughput of a radio stream: the SDIO writes timed out, ESP-Hosted declared
//   the transport unrecoverable and rebooted the whole tablet mid-playback. Updating
//   the slave is the fix, and this is the route that does not need the UART pads.
//
//   The one-shot esp_hosted_slave_ota(url) this used to call is declared but no
//   longer defined in the bundled component -- upstream moved fetching into the
//   caller and left only the chunked begin/write/end/activate API. So the download
//   loop lives here, streaming straight from the HTTP body into the slave rather
//   than buffering a whole image the internal heap has no room for.
//
//   Blocking by design: the shell is unusable for the duration, which is the honest
//   representation of what is happening to the board.
static const size_t COPROC_OTA_CHUNK = 4096;

static void wifiCoprocessorOta(const String& url) {
    if (wifiIsConnected() != 1) {
        outLine("wifi: coproc-ota needs a working connection to fetch the image", C_RED);
        return;
    }
    if (url.length() == 0) {
        outLine("Usage: wifi coproc-ota <url to esp32c6 ESP-Hosted slave image>");
        return;
    }

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;
    http.setTimeout(15000);
    if (url.startsWith("https://")) {
        secureClient.setInsecure();
        if (!http.begin(secureClient, url)) {
            outLine("wifi: could not open " + url, C_RED);
            return;
        }
    } else if (!http.begin(plainClient, url)) {
        outLine("wifi: could not open " + url, C_RED);
        return;
    }

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        outLine("wifi: image fetch returned HTTP " + String(httpCode), C_RED);
        http.end();
        return;
    }
    const int imageSize = http.getSize();

    outLine("wifi: updating co-processor from " + url, C_PINK);
    outLine("wifi: do not power the tablet off until this reports a result.", C_YELLOW);
    Serial.printf("[coproc] slave OTA from %s (%d bytes)\n", url.c_str(), imageSize);

    uint8_t* buffer = (uint8_t*)malloc(COPROC_OTA_CHUNK);
    if (buffer == NULL) {
        outLine("wifi: out of memory for the OTA buffer", C_RED);
        http.end();
        return;
    }

    esp_err_t result = esp_hosted_slave_ota_begin();
    if (result != ESP_OK) {
        outLine("wifi: co-processor refused OTA start: " + String(esp_err_to_name(result)), C_RED);
        free(buffer);
        http.end();
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t written = 0;
    int lastReportedPercent = -1;
    while (http.connected() && (imageSize < 0 || written < (size_t)imageSize)) {
        const size_t available = stream->available();
        if (available == 0) {
            if (!stream->connected()) {
                break;
            }
            delay(1);
            continue;
        }
        const int read = stream->readBytes(buffer, min(available, COPROC_OTA_CHUNK));
        if (read <= 0) {
            continue;
        }
        result = esp_hosted_slave_ota_write(buffer, (uint32_t)read);
        if (result != ESP_OK) {
            break;
        }
        written += read;

        //progress on the panel: a multi-megabyte transfer over SDIO is slow enough
        //that a silent shell reads as a hang
        if (imageSize > 0) {
            const int percent = (int)((written * 100) / (size_t)imageSize);
            if (percent >= lastReportedPercent + 10) {
                lastReportedPercent = percent - (percent % 10);
                outLine("wifi: " + String(lastReportedPercent) + "% (" + String((unsigned)written) + " bytes)");
                drawDisplayFrame();
            }
        }
    }
    free(buffer);
    http.end();

    if (result != ESP_OK) {
        outLine("wifi: co-processor write failed: " + String(esp_err_to_name(result)), C_RED);
        Serial.printf("[coproc] slave OTA write failed after %u bytes: %s\n",
                      (unsigned)written, esp_err_to_name(result));
        return;
    }
    if (imageSize > 0 && written < (size_t)imageSize) {
        outLine("wifi: transfer truncated at " + String((unsigned)written) + "/" + String(imageSize)
                + " bytes; co-processor left on its old firmware", C_RED);
        return;
    }

    result = esp_hosted_slave_ota_end();
    if (result != ESP_OK) {
        outLine("wifi: co-processor rejected the image: " + String(esp_err_to_name(result)), C_RED);
        Serial.printf("[coproc] slave OTA end failed: %s\n", esp_err_to_name(result));
        return;
    }

    //activate reboots the co-processor, which drops the SDIO link out from under the
    //host -- expect Wi-Fi to disappear here. Reboot the tablet afterwards so
    //ESP-Hosted renegotiates the transport from a clean state.
    outLine("wifi: image accepted (" + String((unsigned)written) + " bytes), activating...", C_GREEN);
    result = esp_hosted_slave_ota_activate();
    if (result != ESP_OK) {
        outLine("wifi: activate failed: " + String(esp_err_to_name(result)), C_RED);
        return;
    }
    outLine("wifi: co-processor updated. Reboot the tablet to renegotiate the link.", C_GREEN);
    Serial.println("[coproc] slave OTA activated");
}

//Expected forms: wifi | wifi scan | wifi connect <ssid> <password> | wifi save <ssid> <password>
void handleWifiCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        showWifiStatus();
        return;
    }

    if (parts[1] == "scan") {
        scanWifiNetworks();
        return;
    }

    if (parts[1] == "coproc-ota") {
        wifiCoprocessorOta(partCount > 2 ? parts[2] : String(""));
        return;
    }

    if (parts[1] == "connect") {
        if (partCount < 4) {
            String savedSsid, savedPassword;
            if (loadWifiCredentials(savedSsid, savedPassword)) {
                connectWifiNetwork(savedSsid, savedPassword);
            } else {
                outLine("Usage: wifi connect <ssid> <password>");
            }
            return;
        }
        connectWifiNetwork(parts[2], parts[3]);
        return;
    }

    if (parts[1] == "save") {
        if (partCount < 4) {
            if (wifiIsConnected() != 1) {
                outLine("Usage: wifi save <ssid> <password>");
                return;
            }
            if (saveWifiCredentials(WiFi.SSID(), WiFi.psk())) {
                outLine("Saved WiFi credentials");
            } else {
                outLine("Failed to save WiFi credentials", C_RED);
            }
            return;
        }
        if (saveWifiCredentials(parts[2], parts[3])) {
            outLine("Saved WiFi credentials");
        } else {
            outLine("Failed to save WiFi credentials", C_RED);
        }
        return;
    }

    wifiHelp();
}
