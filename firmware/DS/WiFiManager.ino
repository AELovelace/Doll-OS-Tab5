//   WiFiManager.ino
//   Wi-Fi connectivity: DOLL-OS's wifi.ino command subsystem (scan/connect/save/
//   status) driving a plain STA connection, plus the wifi-manager ROM app that
//   front-ends the same store. DOLL-OS originally also ran an always-on softAP
//   here as a fallback telnet path, but AP+STA on the S3's single radio cost too
//   much streaming throughput (Radio.ino audio starved once its buffer drained)
//   and the AP went unused -- the panel + BLE keyboard already cover the
//   no-network case, so STA is now the only mode.
//
//   Credentials are an ordered *list*, not a single pair: /system/conf/wifi.dsys
//   holds up to WIFI_MAX_SAVED_NETWORKS entries and file order is priority order.
//   Every join path (boot, "wifi connect" with no arguments, the ROM app's
//   reconnect) sweeps that list top-down and stops at the first network that
//   accepts us, so one board can follow its owner between home, work and a phone
//   hotspot without being re-provisioned at each stop.
#include <LittleFS.h>

//Priority-ordered saved networks. Tab-separated `ssid<TAB>password` lines: a tab
//can't appear in either field, so unlike the `key=value` shape Alias.ino/Settings.ino
//use it needs no escaping for the '=' and spaces that are legal in an SSID.
static const char* WIFI_NETWORKS_PATH = "/system/conf/wifi.dsys";
static const char* WIFI_NETWORKS_TMP_PATH = "/system/conf/wifi.tmp.dsys";

//Pre-list single-credential file, imported once (see wifiMigrateLegacyCredentials).
static const char* WIFI_LEGACY_CREDS_PATH = "/wifi.cfg";

static const int WIFI_SSID_MAX = 32;        //802.11 SSID limit
static const int WIFI_PASSWORD_MAX = 63;    //WPA2 PSK limit

//An explicit "connect to this one network" waits the full timeout, because the user
//named it and has nothing else to fall back to. A sweep gives each candidate less,
//since the cost of guessing wrong is paid once per saved network.
static const unsigned long WIFI_JOIN_TIMEOUT_MS = 15000;
static const unsigned long WIFI_SWEEP_JOIN_TIMEOUT_MS = 8000;

// --------------------------------------------------------------- saved network store

//boot-time joins happen before any telnet client exists, and the panel mirror isn't
//being pushed yet either, so the serial log is the only place early failures show up.
//Shell-time callers get the same text through outLine's normal path.
static void wifiLog(const String& text, int color) {
    Serial.println(text);
    outLine(text, color);
}

static void wifiLog(const String& text) {
    wifiLog(text, C_WHITE);
}

static bool wifiSaveNetworks(const WifiCredential networks[], int count);

//One-shot import of the single ssid/password pair older firmware kept in /wifi.cfg.
//Runs before the first read of the list rather than at boot, so it also covers a
//filesystem restored from an old backup. The legacy file is removed only once its
//contents are safely in the new store.
static void wifiMigrateLegacyCredentials() {
    static bool migrationChecked = false;
    if (migrationChecked) {
        return;
    }
    migrationChecked = true;

    if (LittleFS.exists(WIFI_NETWORKS_PATH) || !LittleFS.exists(WIFI_LEGACY_CREDS_PATH)) {
        return;
    }

    ledPulseStorageRead(false);
    File file = LittleFS.open(WIFI_LEGACY_CREDS_PATH, "r");
    if (!file) {
        return;
    }
    String ssid = file.readStringUntil('\n');
    String password = file.readStringUntil('\n');
    file.close();
    ssid.trim();
    password.trim();
    if (ssid.length() == 0) {
        return;
    }

    WifiCredential imported[1];
    imported[0].ssid = ssid;
    imported[0].password = password;
    if (wifiSaveNetworks(imported, 1)) {
        ledPulseStorageWrite(false);
        LittleFS.remove(WIFI_LEGACY_CREDS_PATH);
    }
}

//reads the store into `networks` in priority order, returning how many entries landed
int wifiLoadNetworks(WifiCredential networks[], int maxNetworks) {
    wifiMigrateLegacyCredentials();

    ledPulseStorageRead(false);
    File file = LittleFS.open(WIFI_NETWORKS_PATH, "r");
    if (!file) {
        return 0;
    }

    int count = 0;
    while (file.available() && count < maxNetworks) {
        String line = file.readStringUntil('\n');
        ledPulseStorageRead(false);
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }
        if (line.length() == 0 || line[0] == '#') {
            continue;
        }

        String ssid = line;
        String password = "";
        int tab = line.indexOf('\t');
        if (tab >= 0) {
            ssid = line.substring(0, tab);
            //deliberately not trimmed: a passphrase may legitimately start or end with
            //a space, and the tab already delimits it unambiguously
            password = line.substring(tab + 1);
        }
        ssid.trim();
        if (ssid.length() == 0) {
            continue;
        }
        if (ssid.length() > WIFI_SSID_MAX) {
            ssid = ssid.substring(0, WIFI_SSID_MAX);
        }
        if (password.length() > WIFI_PASSWORD_MAX) {
            password = password.substring(0, WIFI_PASSWORD_MAX);
        }

        networks[count].ssid = ssid;
        networks[count].password = password;
        count++;
    }
    file.close();
    return count;
}

//rewrites the whole store through a temp file + rename, the same way Settings.ino and
//Edit.ino save -- a truncating open would lose every saved network if power dropped
//partway through
static bool wifiSaveNetworks(const WifiCredential networks[], int count) {
    if (!ensureSystemConfDirectory()) {
        return false;
    }
    ledPulseStorageWrite(false);
    LittleFS.remove(WIFI_NETWORKS_TMP_PATH);
    File file = LittleFS.open(WIFI_NETWORKS_TMP_PATH, "w");
    if (!file) {
        return false;
    }

    file.println("# DOLL-OS saved Wi-Fi networks -- first one that answers wins");
    file.println("# Format: ssid<TAB>password");
    for (int i = 0; i < count; i++) {
        if (networks[i].ssid.length() == 0) {
            continue;
        }
        file.print(networks[i].ssid);
        file.print("\t");
        file.println(networks[i].password);
        ledPulseStorageWrite(false);
    }
    file.close();

    ledPulseStorageWrite(false);
    LittleFS.remove(WIFI_NETWORKS_PATH);
    if (!LittleFS.rename(WIFI_NETWORKS_TMP_PATH, WIFI_NETWORKS_PATH)) {
        LittleFS.remove(WIFI_NETWORKS_TMP_PATH);
        return false;
    }
    return true;
}

static int wifiFindNetwork(const WifiCredential networks[], int count, const String& ssid) {
    for (int i = 0; i < count; i++) {
        if (networks[i].ssid == ssid) {
            return i;
        }
    }
    return -1;
}

int wifiSavedNetworkCount() {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    return wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
}

//adds a network, or updates the password of one already saved (keeping its priority).
//`added` reports which of the two happened so callers can word their own message.
bool wifiAddNetwork(const String& ssid, const String& password, bool& added) {
    added = false;
    String trimmedSsid = ssid;
    trimmedSsid.trim();
    if (trimmedSsid.length() == 0 || trimmedSsid.length() > WIFI_SSID_MAX) {
        return false;
    }
    if (password.length() > WIFI_PASSWORD_MAX) {
        return false;
    }

    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    int index = wifiFindNetwork(networks, count, trimmedSsid);
    if (index < 0) {
        if (count >= WIFI_MAX_SAVED_NETWORKS) {
            return false;
        }
        index = count++;
        added = true;
    }
    networks[index].ssid = trimmedSsid;
    networks[index].password = password;
    return wifiSaveNetworks(networks, count);
}

bool wifiForgetNetwork(const String& ssid) {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    int index = wifiFindNetwork(networks, count, ssid);
    if (index < 0) {
        return false;
    }
    for (int i = index; i < count - 1; i++) {
        networks[i] = networks[i + 1];
    }
    count--;
    return wifiSaveNetworks(networks, count);
}

//moves the entry at `from` to position `to`, both 0-based, shifting the rest along.
//This is the only way to change which network wins when two are in range at once.
bool wifiMoveNetwork(int from, int to) {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return false;
    }

    WifiCredential moved = networks[from];
    if (to < from) {
        for (int i = from; i > to; i--) {
            networks[i] = networks[i - 1];
        }
    } else {
        for (int i = from; i < to; i++) {
            networks[i] = networks[i + 1];
        }
    }
    networks[to] = moved;
    return wifiSaveNetworks(networks, count);
}

//compatibility shims for the pre-list API (see docs/api-guide.md): "the" credential is
//just the highest-priority saved network, and saving one adds to the list
bool loadWifiCredentials(String& ssid, String& password) {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    if (count == 0) {
        return false;
    }
    ssid = networks[0].ssid;
    password = networks[0].password;
    return ssid.length() > 0;
}

bool saveWifiCredentials(const String& ssid, const String& password) {
    bool added = false;
    return wifiAddNetwork(ssid, password, added);
}

// ------------------------------------------------------------------------- joining

int wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED ? 1 : 0;
}

//Single-network join -- the one place that actually drives the radio.
//
//Abort any association still in flight before begin(): a stuck/failed join leaves the
//STA "connecting", and esp_wifi_set_config() (inside begin()) is rejected in that state
//("cannot set config"), so the join would silently fail. disconnect() (radio stays
//started) clears the pending attempt; the short settle lets the async stop land before
//set_config runs. With driver auto-reconnect off (connectToInternet), nothing
//re-associates underneath us between the two calls.
static bool wifiJoinNetwork(const String& ssid, const String& password, unsigned long timeoutMs) {
    ledPulseNetwork();
    ledSetWifiConnected(false);

    WiFi.disconnect();
    delay(200);
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long startTime = millis();
    while (millis() - startTime < timeoutMs) {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            ledSetWifiConnected(true);
            return true;
        }
        //A sweep of ten networks can't afford to sit out the full timeout on each one it
        //was never going to join, and these two answers are terminal: the AP isn't there,
        //or it rejected our key. Everything else (WL_IDLE_STATUS, WL_DISCONNECTED) is a
        //normal intermediate state mid-association and has to keep waiting.
        if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) {
            break;
        }
        delay(100);
    }

    bool connected = wifiIsConnected() == 1;
    ledSetWifiConnected(connected);
    return connected;
}

//Fills `count` with the scan size and returns true if the scan ran. Results stay live
//for WiFi.SSID(i) etc. until the caller calls WiFi.scanDelete().
static bool wifiRunScan(int& count) {
    count = 0;
    //A scan can't start while the STA is mid-association: esp_wifi_scan_start() bails out
    //and scanNetworks() fails *immediately* (WIFI_SCAN_FAILED) instead of taking its usual
    //~2s. If we're not actually connected, abort any pending association so the radio is
    //free to scan. (A live connection scans fine -- leave it alone.) The scan itself is
    //synchronous, so maintainInternetConnection() can't re-associate underneath us until
    //it returns.
    if (wifiIsConnected() != 1) {
        WiFi.disconnect();
        delay(100);
    }
    int found = WiFi.scanNetworks();
    if (found < 0) {
        WiFi.scanDelete();
        return false;
    }
    count = found;
    return true;
}

static bool wifiSsidInScan(const String& ssid, int scanCount) {
    for (int i = 0; i < scanCount; i++) {
        if (WiFi.SSID(i) == ssid) {
            return true;
        }
    }
    return false;
}

//Walks the saved list top-down and stops at the first network that lets us in.
//
//Two passes, not one: the first tries only networks the scan just saw, so being out of
//range costs nothing, and the second tries whatever is left over. That second pass is
//what covers a hidden SSID (which never shows up in scan results at all) and the case
//where the scan itself failed -- without it, a hidden home network would have become
//unjoinable the moment this replaced the old single-credential path. Priority is still
//the file's order within each pass; the scan only decides who goes first, never who wins.
bool connectToSavedNetworks(bool verbose) {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    if (count == 0) {
        return false;
    }

    int scanCount = 0;
    bool scanned = wifiRunScan(scanCount);

    bool tried[WIFI_MAX_SAVED_NETWORKS];
    for (int i = 0; i < count; i++) {
        tried[i] = false;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            if (tried[i]) {
                continue;
            }
            if (pass == 0 && (!scanned || !wifiSsidInScan(networks[i].ssid, scanCount))) {
                continue;
            }
            tried[i] = true;

            if (verbose) {
                wifiLog("wifi: trying " + networks[i].ssid +
                        " (" + String(i + 1) + " of " + String(count) + ")");
            }
            if (wifiJoinNetwork(networks[i].ssid, networks[i].password, WIFI_SWEEP_JOIN_TIMEOUT_MS)) {
                WiFi.scanDelete();
                return true;
            }
        }
    }

    WiFi.scanDelete();
    return false;
}

//tries every saved network in priority order, falling back to the config.h defaults
//returns true if any of them let us on
bool connectToInternet() {
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

    ledPulseNetwork();
    ledSetWifiConnected(false);

    if (connectToSavedNetworks(true)) {
        ledSetWifiConnected(true);
        wifiLog("Connected to " + WiFi.SSID() + " -- " + WiFi.localIP().toString(), C_GREEN);
        return true;
    }

    //Nothing saved answered. The compiled-in default is the last resort -- skipped when
    //it's already in the list, since the sweep above just tried it.
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    if (wifiFindNetwork(networks, count, String(STA_DEFAULT_SSID)) < 0) {
        wifiLog("wifi: trying built-in default " + String(STA_DEFAULT_SSID));
        if (wifiJoinNetwork(STA_DEFAULT_SSID, STA_DEFAULT_PASSWORD, WIFI_JOIN_TIMEOUT_MS)) {
            ledSetWifiConnected(true);
            wifiLog("Connected to " + WiFi.SSID() + " -- " + WiFi.localIP().toString(), C_GREEN);
            return true;
        }
    }

    wifiLog("Could not connect to any known network.", C_RED);
    ledSetWifiConnected(false);
    return false;
}

unsigned long previousReconnectAttempt = 0;
const unsigned long reconnectInterval = 10000;

//How many failed 10s ticks before we stop retrying the current network and point the
//radio at the next saved one. ~60s is long enough that a rebooting router isn't mistaken
//for a move, short enough that carrying the board to another saved network gets noticed.
static const int WIFI_TICKS_BEFORE_ROTATE = 6;
static int wifiReconnectTicks = 0;
static int wifiRotationIndex = 0;

void maintainInternetConnection() {
    if (WiFi.status() == WL_CONNECTED) {
        ledSetWifiConnected(true);
        wifiReconnectTicks = 0;
        return;
    }
    ledSetWifiConnected(false);
    if (millis() - previousReconnectAttempt < reconnectInterval) {
        return;
    }
    previousReconnectAttempt = millis();
    ledPulseNetwork();
    wifiReconnectTicks++;

    //After a minute of getting nowhere, assume the network we were on isn't the one we're
    //standing in and re-aim at the next saved entry. Only the config changes here -- we
    //don't wait on the result, because this runs inside loop() and inside
    //appRuntimeYield(), where a blocking sweep would stall the shell and any running app.
    //The ticks that follow re-associate against whatever we last pointed at, so rotation
    //keeps stepping through the list until something answers.
    if (wifiReconnectTicks >= WIFI_TICKS_BEFORE_ROTATE) {
        wifiReconnectTicks = 0;
        WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
        int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
        if (count > 1) {
            wifiRotationIndex = (wifiRotationIndex + 1) % count;
            WiFi.disconnect();
            delay(200);
            WiFi.begin(networks[wifiRotationIndex].ssid.c_str(),
                       networks[wifiRotationIndex].password.c_str());
            return;
        }
    }

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

// ----------------------------------------------------------------------- reporting

void scanWifiNetworks() {
    WiFi.scanDelete();
    outLine("Scanning for Wifi Networks");
    telnetClient.flush();   //push this line out before the blocking scan begins
    ledPulseNetwork();

    int networkCount = 0;
    if (!wifiRunScan(networkCount)) {
        outLine("Scan Failed", C_RED);
        return;
    }
    if (networkCount == 0) {
        outLine("No Networks Found");
        WiFi.scanDelete();
        return;
    }
    outLine(String(networkCount) + " Networks Found");

    WifiCredential saved[WIFI_MAX_SAVED_NETWORKS];
    int savedCount = wifiLoadNetworks(saved, WIFI_MAX_SAVED_NETWORKS);

    for (int i = 0; i < networkCount; i++) {
        String ssid = WiFi.SSID(i);
        bool isSaved = ssid.length() > 0 && wifiFindNetwork(saved, savedCount, ssid) >= 0;
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
        if (isSaved) {
            result += " [saved]";
        }
        outLine(result, isSaved ? C_GREEN : C_WHITE);
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

//prints the saved list in priority order, marking the one we're actually on
static void wifiListSavedNetworks() {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    outLine("Saved networks (" + String(count) + " of " + String(WIFI_MAX_SAVED_NETWORKS) + ")", C_CYAN);
    if (count == 0) {
        outLine("(none -- use 'wifi save <ssid> <password>')");
        return;
    }
    String current = wifiIsConnected() == 1 ? WiFi.SSID() : "";
    for (int i = 0; i < count; i++) {
        bool isCurrent = current.length() > 0 && networks[i].ssid == current;
        String line = String(i + 1) + ". " + networks[i].ssid;
        if (networks[i].password.length() == 0) {
            line += "  (open)";
        }
        if (isCurrent) {
            line += "  <- connected";
        }
        outLine(line, isCurrent ? C_GREEN : C_WHITE);
    }
    outLine("Connect order is top-down; 'wifi move <from> <to>' re-ranks.", C_CYAN);
}

void connectWifiNetwork(const String& ssid, const String& password) {
    outLine("Connecting to: " + ssid);
    telnetClient.flush();

    if (wifiJoinNetwork(ssid, password, WIFI_JOIN_TIMEOUT_MS)) {
        wifiStatus();
        return;
    }
    ledPulseError();
    outLine("WiFi connect failed", C_RED);
}

// ------------------------------------------------------------------- shell command

void wifiHelp() {
    outLine("WiFi subcommands:");
    outLine("wifi                          show connection status");
    outLine("wifi scan                     list networks in range");
    outLine("wifi list                     list saved networks, in connect order");
    outLine("wifi connect                  try every saved network");
    outLine("wifi connect <ssid>           connect using its saved password");
    outLine("wifi connect <ssid> <pass>    connect without saving");
    outLine("wifi save [<ssid> <pass>]     save a network (defaults to the live one)");
    outLine("wifi forget <ssid|number>     remove a saved network");
    outLine("wifi move <from> <to>         change a saved network's priority");
    outLine("wifi manager                  open the wifi-manager app");
    outLine("Quote values containing spaces: wifi save \"My Net\" \"my pass\"");
}

//resolves a "which saved network" argument, accepting either the 1-based number
//"wifi list" prints or the SSID itself. Returns -1 if it matches neither.
static int wifiResolveSavedArgument(const WifiCredential networks[], int count, const String& argument) {
    int asNumber = argument.toInt();
    if (asNumber >= 1 && asNumber <= count && argument == String(asNumber)) {
        return asNumber - 1;
    }
    return wifiFindNetwork(networks, count, argument);
}

//Expected forms: see wifiHelp()
void handleWifiCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        showWifiStatus();
        return;
    }

    String sub = parts[1];
    sub.toLowerCase();

    if (sub == "scan") {
        scanWifiNetworks();
        return;
    }

    if (sub == "list") {
        wifiListSavedNetworks();
        return;
    }

    if (sub == "manager" || sub == "app") {
        runWifiManagerApp();
        return;
    }

    if (sub == "connect") {
        if (partCount < 3) {
            //no target named: sweep everything we know
            if (wifiSavedNetworkCount() == 0) {
                outLine("No saved networks. Usage: wifi connect <ssid> <password>", C_RED);
                return;
            }
            outLine("Trying saved networks...", C_CYAN);
            telnetClient.flush();
            if (connectToSavedNetworks(true)) {
                wifiStatus();
            } else {
                ledPulseError();
                outLine("No saved network accepted the connection", C_RED);
            }
            return;
        }

        if (partCount < 4) {
            //one argument: only useful if we already hold that network's password
            WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
            int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
            int index = wifiResolveSavedArgument(networks, count, parts[2]);
            if (index < 0) {
                outLine("wifi: " + parts[2] + " is not saved", C_RED);
                outLine("Usage: wifi connect <ssid> <password>");
                return;
            }
            connectWifiNetwork(networks[index].ssid, networks[index].password);
            return;
        }

        connectWifiNetwork(parts[2], parts[3]);
        return;
    }

    if (sub == "save") {
        String ssid;
        String password;
        if (partCount < 4) {
            if (wifiIsConnected() != 1) {
                outLine("Usage: wifi save <ssid> <password>");
                return;
            }
            ssid = WiFi.SSID();
            password = WiFi.psk();
        } else {
            ssid = parts[2];
            password = parts[3];
        }

        bool added = false;
        if (!wifiAddNetwork(ssid, password, added)) {
            if (wifiSavedNetworkCount() >= WIFI_MAX_SAVED_NETWORKS) {
                outLine("wifi: the saved list is full (" + String(WIFI_MAX_SAVED_NETWORKS) +
                        ") -- forget one first", C_RED);
            } else {
                outLine("Failed to save WiFi credentials", C_RED);
            }
            return;
        }
        outLine(added ? ("Saved " + ssid) : ("Updated password for " + ssid), C_GREEN);
        return;
    }

    if (sub == "forget") {
        if (partCount < 3) {
            outLine("Usage: wifi forget <ssid|number>", C_RED);
            return;
        }
        WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
        int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
        int index = wifiResolveSavedArgument(networks, count, parts[2]);
        if (index < 0) {
            outLine("wifi: " + parts[2] + " is not saved", C_RED);
            return;
        }
        String ssid = networks[index].ssid;
        if (!wifiForgetNetwork(ssid)) {
            outLine("wifi: could not remove " + ssid, C_RED);
            return;
        }
        outLine("Forgot " + ssid, C_GREEN);
        return;
    }

    if (sub == "move") {
        if (partCount < 4) {
            outLine("Usage: wifi move <from> <to>   (positions from 'wifi list')", C_RED);
            return;
        }
        WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
        int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
        int from = wifiResolveSavedArgument(networks, count, parts[2]);
        int to = parts[3].toInt() - 1;
        if (from < 0 || to < 0 || to >= count) {
            outLine("wifi: positions must be 1-" + String(count), C_RED);
            return;
        }
        if (!wifiMoveNetwork(from, to)) {
            outLine("wifi: nothing to move", C_YELLOW);
            return;
        }
        wifiListSavedNetworks();
        return;
    }

    wifiHelp();
}

// ------------------------------------------------------------- wifi-manager ROM app

//   The one app that has to work before anything else does: with no network there is
//   no telnet, no FTP and no dappstore, so a network editor that lived on the SD card
//   or in LittleFS could be missing exactly when it is needed. So it is native and
//   compiled in rather than shipped as a .dapp, and "apps" lists it under /rom/apps.
//
//   The UI is the same blocking prompt loop Radio.ino and the .dapp INPUT opcode use:
//   read from telnet and the DS-Slave keyboard bridge in turn, mirror the buffer into
//   the panel's command bar, and keep the rest of the system serviced between
//   keystrokes.

static const char* WIFI_APP_PROMPT_CANCEL = "\x01";   //sentinel a prompt returns for "go back"

//Everything loop() does except maintainInternetConnection(): the app drives the radio
//itself (scan, join), and a 10s reconnect tick firing in the middle of a scan the user
//asked for would either fail that scan outright or silently re-associate underneath a
//connection they just chose.
static void wifiAppServiceUi() {
    ftpService();
    radioService();
    ledService();
    drawDisplayFrame();
    delay(1);
}

//blocking line read. Returns WIFI_APP_PROMPT_CANCEL when the user submits an empty line
//or "q", which every caller treats as "back out of this screen".
static String wifiAppPrompt(const String& prompt, bool masked) {
    String answer = "";
    commandCursorPos = 0;
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(prompt);
    }
    setActiveInput(prompt, answer, masked);

    while (true) {
        LineInputResult r = readLineEditedInput(answer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(answer);
        }
        setActiveInput(prompt, answer, masked);
        wifiAppServiceUi();

        if (r != LINE_SUBMITTED) {
            continue;
        }

        String submitted = answer;
        answer = "";
        commandCursorPos = 0;
        submitted.trim();
        outLine(prompt + (masked ? "[hidden]" : submitted), C_CYAN);
        setActiveInput(shellPrompt(), "", false);

        if (submitted.length() == 0 || submitted == "q" || submitted == "Q") {
            return String(WIFI_APP_PROMPT_CANCEL);
        }
        return submitted;
    }
}

static bool wifiAppCancelled(const String& answer) {
    return answer == WIFI_APP_PROMPT_CANCEL;
}

//A password prompt that accepts an explicitly empty passphrase, which the shared
//wifiAppPrompt() cannot express -- there, empty means "back out". Enter alone means
//"this network is open"; "q" is still the way out.
static String wifiAppPromptPassword(bool& cancelled) {
    cancelled = false;
    String answer = "";
    const String prompt = "password (blank if open)> ";
    commandCursorPos = 0;
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(prompt);
    }
    setActiveInput(prompt, answer, true);

    while (true) {
        LineInputResult r = readLineEditedInput(answer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(answer);
        }
        setActiveInput(prompt, answer, true);
        wifiAppServiceUi();

        if (r != LINE_SUBMITTED) {
            continue;
        }

        String submitted = answer;
        answer = "";
        commandCursorPos = 0;
        outLine(prompt + "[hidden]", C_CYAN);
        setActiveInput(shellPrompt(), "", false);
        if (submitted == "q" || submitted == "Q") {
            cancelled = true;
            return "";
        }
        return submitted;
    }
}

static void wifiAppPause() {
    wifiAppPrompt("press Enter to continue> ", false);
}

//saving a network and connecting to it are one action as far as anyone standing in
//front of the board is concerned
static void wifiAppSaveAndConnect(const String& ssid, const String& password) {
    bool added = false;
    if (!wifiAddNetwork(ssid, password, added)) {
        if (wifiSavedNetworkCount() >= WIFI_MAX_SAVED_NETWORKS) {
            outLine("The list is full (" + String(WIFI_MAX_SAVED_NETWORKS) +
                    " networks). Forget one first.", C_RED);
        } else {
            outLine("Could not save " + ssid, C_RED);
        }
        wifiAppPause();
        return;
    }
    outLine(added ? ("Saved " + ssid) : ("Updated " + ssid), C_GREEN);
    connectWifiNetwork(ssid, password);
    wifiAppPause();
}

//scan, pick one by number, type its password
static void wifiAppAddFromScan() {
    outClearScreen();
    outLine("SCAN", C_PINK);
    outLine("Scanning...");
    telnetClient.flush();
    drawDisplayFrame();

    int scanCount = 0;
    if (!wifiRunScan(scanCount)) {
        outLine("Scan failed -- try again in a moment", C_RED);
        wifiAppPause();
        return;
    }
    if (scanCount == 0) {
        outLine("No networks in range", C_YELLOW);
        WiFi.scanDelete();
        wifiAppPause();
        return;
    }

    WifiCredential saved[WIFI_MAX_SAVED_NETWORKS];
    int savedCount = wifiLoadNetworks(saved, WIFI_MAX_SAVED_NETWORKS);
    for (int i = 0; i < scanCount; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            outLine(String(i + 1) + ". <hidden>  " + String(WiFi.RSSI(i)) + "dBm");
            continue;
        }
        bool isSaved = wifiFindNetwork(saved, savedCount, ssid) >= 0;
        String line = String(i + 1) + ". " + ssid + "  " + String(WiFi.RSSI(i)) + "dBm";
        if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
            line += " open";
        }
        if (isSaved) {
            line += " [saved]";
        }
        outLine(line, isSaved ? C_GREEN : C_WHITE);
    }

    String answer = wifiAppPrompt("network number (q to go back)> ", false);
    if (wifiAppCancelled(answer)) {
        WiFi.scanDelete();
        return;
    }

    int choice = answer.toInt();
    if (choice < 1 || choice > scanCount) {
        outLine("Pick a number from 1 to " + String(scanCount), C_RED);
        WiFi.scanDelete();
        wifiAppPause();
        return;
    }

    //copied out before scanDelete: WiFi.SSID(i) reads from the scan result table, and
    //the password prompt below runs long enough that holding that table is pointless
    String ssid = WiFi.SSID(choice - 1);
    bool isOpen = WiFi.encryptionType(choice - 1) == WIFI_AUTH_OPEN;
    WiFi.scanDelete();

    if (ssid.length() == 0) {
        //a hidden AP puts no SSID in its beacon, so there is nothing here to select --
        //it has to be typed in by name from the "add by hand" screen
        outLine("That network hides its name -- add it by hand instead", C_YELLOW);
        wifiAppPause();
        return;
    }

    String password = "";
    if (!isOpen) {
        bool cancelled = false;
        password = wifiAppPromptPassword(cancelled);
        if (cancelled) {
            return;
        }
    }
    wifiAppSaveAndConnect(ssid, password);
}

//for hidden networks, and for anything the scan did not turn up
static void wifiAppAddByHand() {
    outClearScreen();
    outLine("ADD A NETWORK", C_PINK);
    outLine("Type the name exactly -- SSIDs are case sensitive.");
    String ssid = wifiAppPrompt("ssid (q to go back)> ", false);
    if (wifiAppCancelled(ssid)) {
        return;
    }
    bool cancelled = false;
    String password = wifiAppPromptPassword(cancelled);
    if (cancelled) {
        return;
    }
    wifiAppSaveAndConnect(ssid, password);
}

//shared picker for the three screens that act on one saved entry. Fills `networks`
//and `count` so the caller can use the entry it picked without re-reading the file.
static int wifiAppPickSaved(const String& title, WifiCredential networks[], int& count) {
    count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    outClearScreen();
    outLine(title, C_PINK);
    if (count == 0) {
        outLine("Nothing saved yet", C_YELLOW);
        wifiAppPause();
        return -1;
    }

    String current = wifiIsConnected() == 1 ? WiFi.SSID() : "";
    for (int i = 0; i < count; i++) {
        bool isCurrent = current.length() > 0 && networks[i].ssid == current;
        outLine(String(i + 1) + ". " + networks[i].ssid + (isCurrent ? "  <- connected" : ""),
                isCurrent ? C_GREEN : C_WHITE);
    }

    String answer = wifiAppPrompt("number (q to go back)> ", false);
    if (wifiAppCancelled(answer)) {
        return -1;
    }
    int choice = answer.toInt();
    if (choice < 1 || choice > count) {
        outLine("Pick a number from 1 to " + String(count), C_RED);
        wifiAppPause();
        return -1;
    }
    return choice - 1;
}

static void wifiAppConnectOne() {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = 0;
    int index = wifiAppPickSaved("CONNECT TO ONE NETWORK", networks, count);
    if (index < 0) {
        return;
    }
    connectWifiNetwork(networks[index].ssid, networks[index].password);
    wifiAppPause();
}

static void wifiAppForget() {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = 0;
    int index = wifiAppPickSaved("FORGET A NETWORK", networks, count);
    if (index < 0) {
        return;
    }
    String ssid = networks[index].ssid;
    String confirm = wifiAppPrompt("forget " + ssid + "? (y/N)> ", false);
    if (wifiAppCancelled(confirm) || (confirm != "y" && confirm != "Y")) {
        outLine("Kept " + ssid);
        wifiAppPause();
        return;
    }
    if (wifiForgetNetwork(ssid)) {
        outLine("Forgot " + ssid, C_GREEN);
    } else {
        outLine("Could not remove " + ssid, C_RED);
    }
    wifiAppPause();
}

static void wifiAppReorder() {
    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = 0;
    int index = wifiAppPickSaved("CHANGE PRIORITY", networks, count);
    if (index < 0) {
        return;
    }
    outLine("Position 1 is tried first.");
    String answer = wifiAppPrompt("new position for " + networks[index].ssid +
                                  " (1-" + String(count) + ")> ", false);
    if (wifiAppCancelled(answer)) {
        return;
    }
    int target = answer.toInt() - 1;
    if (target < 0 || target >= count) {
        outLine("Pick a position from 1 to " + String(count), C_RED);
        wifiAppPause();
        return;
    }
    if (!wifiMoveNetwork(index, target)) {
        outLine("Nothing changed", C_YELLOW);
        wifiAppPause();
        return;
    }
    outLine("Moved " + networks[index].ssid + " to position " + String(target + 1), C_GREEN);
    wifiAppPause();
}

static void wifiAppSweep() {
    outClearScreen();
    outLine("CONNECT", C_PINK);
    if (wifiSavedNetworkCount() == 0) {
        outLine("Nothing saved yet -- scan and add a network first", C_YELLOW);
        wifiAppPause();
        return;
    }
    outLine("Trying saved networks in order...");
    telnetClient.flush();
    drawDisplayFrame();

    if (connectToSavedNetworks(true)) {
        wifiStatus();
    } else {
        ledPulseError();
        outLine("None of the saved networks answered", C_RED);
    }
    wifiAppPause();
}

static void wifiAppDrawMenu() {
    outClearScreen();
    outLine("========================================", C_PINK);
    outLine("              WIFI MANAGER", C_PINK);
    outLine("========================================", C_PINK);

    if (wifiIsConnected() == 1) {
        outLine("On " + WiFi.SSID() + " -- " + WiFi.localIP().toString(), C_GREEN);
    } else {
        outLine("Not connected", C_RED);
    }

    WifiCredential networks[WIFI_MAX_SAVED_NETWORKS];
    int count = wifiLoadNetworks(networks, WIFI_MAX_SAVED_NETWORKS);
    outLine(String(count) + " of " + String(WIFI_MAX_SAVED_NETWORKS) + " networks saved, tried in this order:", C_CYAN);
    String current = wifiIsConnected() == 1 ? WiFi.SSID() : "";
    for (int i = 0; i < count; i++) {
        bool isCurrent = current.length() > 0 && networks[i].ssid == current;
        outLine("  " + String(i + 1) + ". " + networks[i].ssid + (isCurrent ? "  <-" : ""),
                isCurrent ? C_GREEN : C_WHITE);
    }

    outLine("");
    outLine("1  scan and add a network");
    outLine("2  add a network by hand");
    outLine("3  connect (try the whole list)");
    outLine("4  connect to one saved network");
    outLine("5  forget a network");
    outLine("6  change priority");
    outLine("q  back to DOLL-OS");
}

void runWifiManagerApp() {
    while (true) {
        wifiAppDrawMenu();
        String choice = wifiAppPrompt("wifi> ", false);
        if (wifiAppCancelled(choice)) {
            break;
        }
        if (choice == "1") {
            wifiAppAddFromScan();
        } else if (choice == "2") {
            wifiAppAddByHand();
        } else if (choice == "3") {
            wifiAppSweep();
        } else if (choice == "4") {
            wifiAppConnectOne();
        } else if (choice == "5") {
            wifiAppForget();
        } else if (choice == "6") {
            wifiAppReorder();
        }
    }

    outClearScreen();
    setActiveInput(shellPrompt(), "", false);
    showWifiStatus();
}
