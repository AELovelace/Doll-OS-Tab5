//   FtpServer.ino
//   exposes the mounted SD card to the network as an FTP server -- the practical
//   file-transfer path for this board. Firmware USB mass storage is deliberately
//   disabled, so FTP moves files on/off the card from a PC over
//   the WiFi that's already up, and every desktop OS can browse it: in Windows
//   Explorer's address bar type  ftp://<station-ip>/  (or use FileZilla/WinSCP).
//
//   It does not block: the server is serviced one non-blocking tick
//   at a time from loop() (ftpService below, next to readTelnetClient()), so the
//   shell, panel, radio and telnet all keep working while a transfer is in flight --
//   large files just get chunked across many loop() iterations. Toggle it with the
//   "ftp" command; it stays off until asked for.
//
//   Storage backend + credentials: the library is a separately-compiled translation
//   unit, so the SD_MMC selection can't be made from this file -- it comes from
//   -DDEFAULT_STORAGE_TYPE_ESP32=STORAGE_SD_MMC in platformio.ini's build_flags,
//   which every translation unit including FtpServer.h sees. Without it the library
//   falls back to its own default of STORAGE_FFAT and serves an unmounted FFat
//   partition instead of the card. begin() does NOT re-mount SD_MMC -- it uses the
//   card Storage.ino already mounted with this board's custom pins. Creds default
//   from config.h (FTP_USER/FTP_PASS) but can be overridden without recompiling via
//   "settings set ftp.user/ftp.pass" (Settings.ino).
#include <Arduino.h>
#include <SimpleFTPServer.h>
#include <esp_heap_caps.h>
#include <new>

//command port 21, passive data port 50009 -- the library defaults (FtpServer.h)
//FtpServer embeds its fallback transfer buffer plus several 263-byte path/command
//buffers. Keeping the object static charged all of that to internal .bss even while
//FTP was off. Construct it lazily in PSRAM instead; FTP is never serviced from an ISR
//and its actual socket buffers still choose whatever capabilities their drivers need.
static FtpServer* ftpSrv = nullptr;
static bool ftpSrvUsesPlacementStorage = false;
static bool ftpActive = false;

//FtpServer::begin() does not copy the credentials -- it stores the two const char*
//it was handed (FtpServer.h: "const char* user; const char* pass;") and dereferences
//them later, when a client actually sends USER/PASS. Passing c_str() of a local
//String left it comparing against freed heap, so every login failed with 530 and the
//client was dropped the moment it connected. These file-scope copies outlive the
//server object, so the pointers stay valid for as long as FTP is up.
static String ftpUserHeld;
static String ftpPassHeld;

static bool ftpEnsureServer() {
    if (ftpSrv != nullptr) {
        return true;
    }

    void* storage = heap_caps_malloc(sizeof(FtpServer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage != nullptr) {
        ftpSrv = new (storage) FtpServer();
        ftpSrvUsesPlacementStorage = true;
        Serial.printf("[psram] ftpServer: %u bytes -> PSRAM\n", (unsigned)sizeof(FtpServer));
        return true;
    }

    ftpSrv = new (std::nothrow) FtpServer();
    ftpSrvUsesPlacementStorage = false;
    if (ftpSrv != nullptr) {
        Serial.printf("[psram] ftpServer: %u bytes -> general heap (PSRAM unavailable)\n",
                      (unsigned)sizeof(FtpServer));
    }
    return ftpSrv != nullptr;
}

static void ftpReleaseServer() {
    if (ftpSrv == nullptr) {
        return;
    }
    if (ftpSrvUsesPlacementStorage) {
        ftpSrv->~FtpServer();
        heap_caps_free(ftpSrv);
    } else {
        delete ftpSrv;
    }
    ftpSrv = nullptr;
    ftpSrvUsesPlacementStorage = false;
}

//serviced every loop() tick while active -- non-blocking, drives the FTP state
//machine one step (accept/auth/one buffer of transfer) and returns immediately
void ftpService() {
    if (ftpActive && ftpSrv != nullptr) {
        ftpSrv->handleFTP();
    }
    ledSetFtpActive(ftpActive);
}

static void ftpStart() {
    if (ftpActive) {
        outLine("ftp: already running", C_YELLOW);
        return;
    }
    if (!sdCardMounted) {
        outLine("ftp: SD card not mounted", C_RED);
        return;
    }
    if (!ftpEnsureServer()) {
        outLine("ftp: not enough memory for server", C_RED);
        return;
    }
    //Creds can be overridden at runtime via "settings set ftp.user/ftp.pass" without
    //recompiling -- see Settings.ino. begin() silently ignores a credential that is
    //empty or >= FTP_CRED_SIZE (16) chars, and its constructor never initialises the
    //pointers, so an out-of-range value would leave the server matching against
    //whatever the PSRAM allocation happened to contain. Reject it here instead.
    String ftpUser = settingsGet("ftp.user", FTP_USER);
    String ftpPass = settingsGet("ftp.pass", FTP_PASS);
    if (ftpUser.length() == 0 || ftpUser.length() >= FTP_CRED_SIZE ||
        ftpPass.length() == 0 || ftpPass.length() >= FTP_CRED_SIZE) {
        outLine("ftp: user and pass must each be 1-" + String(FTP_CRED_SIZE - 1) + " chars", C_RED);
        outLine("  set them with: settings set ftp.user <name> / settings set ftp.pass <pw>");
        ftpReleaseServer();
        return;
    }

    //begin() only starts the listeners + allocates the transfer buffer; the SD card
    //stays mounted exactly as Storage.ino left it. The credential Strings must stay
    //alive for the lifetime of the server -- see ftpUserHeld/ftpPassHeld above.
    ftpUserHeld = ftpUser;
    ftpPassHeld = ftpPass;
    ftpSrv->begin(ftpUserHeld.c_str(), ftpPassHeld.c_str());
    ftpActive = true;
    ledSetFtpActive(true);

    outLine("FTP server on", C_GREEN);
    if (wifiIsConnected() == 1) {
        outLine("  ftp://" + WiFi.localIP().toString() + "/  (Explorer/FileZilla/WinSCP)");
    } else {
        outLine("  (WiFi not connected -- reachable once STA joins)", C_YELLOW);
    }
    outLine("  user: " + ftpUser + "   pass: " + ftpPass);
    outLine("  serving the SD card -- 'ftp off' to stop");
}

static void ftpStop() {
    if (!ftpActive) {
        outLine("ftp: not running", C_YELLOW);
        return;
    }
    ftpSrv->end();
    ftpActive = false;
    ftpReleaseServer();
    ledSetFtpActive(false);
    outLine("FTP server off");
}

static void ftpStatus() {
    if (!ftpActive) {
        outLine("FTP: off  ('ftp on' to start)");
        return;
    }
    outLine("FTP: on", C_GREEN);
    if (wifiIsConnected() == 1) {
        outLine("  ftp://" + WiFi.localIP().toString() + "/");
    }
    outLine("  user: " + settingsGet("ftp.user", FTP_USER) + "   pass: " + settingsGet("ftp.pass", FTP_PASS));
}

//handles the "ftp" command: "ftp on"/"start" begins, "ftp off"/"stop" ends, bare
//"ftp" reports status. Non-modal -- the prompt returns immediately either way.
void handleFtpCommand(const String parts[], int partCount) {
    String sub = (partCount > 1) ? parts[1] : "";
    sub.toLowerCase();

    if (sub == "on" || sub == "start") {
        ftpStart();
    } else if (sub == "off" || sub == "stop") {
        ftpStop();
    } else if (sub == "" || sub == "status") {
        ftpStatus();
    } else {
        outLine("Usage: ftp [on|off|status]");
    }
}
