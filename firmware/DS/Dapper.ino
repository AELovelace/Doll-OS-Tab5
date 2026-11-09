//   Dapper.ino
//   Static-repository package manager for .dapp applications. Dapper talks only
//   to the canonical HTTPS repository, streams its NDJSON catalog, verifies
//   package size + SHA-256 + embedded metadata, and commits installs atomically.
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <time.h>

static const char* DAPPER_REPOSITORY_ID = "sadgirlsclub";
static const char* DAPPER_REPOSITORY_BASE_URL = "https://sadgirlsclub.wtf/dapper/";
static const char* DAPPER_REPOSITORY_URL = "https://sadgirlsclub.wtf/dapper/repo.json";
static const char* DAPPER_STATE_DIR = "/.dapper";
static const char* DAPPER_CATALOG_PATH = "/.dapper/catalog-v1.ndjson";
static const char* DAPPER_CATALOG_PART_PATH = "/.dapper/catalog.part";
static const char* DAPPER_REPO_PART_PATH = "/.dapper/repo.part";
static const char* DAPPER_INSTALLED_PATH = "/.dapper/installed.ndjson";
static const char* DAPPER_INSTALLED_PART_PATH = "/.dapper/installed.part";
static const char* DAPPER_PACKAGE_PART_PATH = "/.dapper/package.part";
static const size_t DAPPER_MAX_REPO_BYTES = 4096;
static const size_t DAPPER_MAX_CATALOG_BYTES = 128 * 1024;
static const size_t DAPPER_MAX_PACKAGE_BYTES = 256 * 1024;
static const size_t DAPPER_MAX_CATALOG_LINE = 4096;
static const size_t DAPPER_IO_BUFFER_BYTES = 512;
static const unsigned long DAPPER_HTTP_IDLE_TIMEOUT_MS = 15000;
static const unsigned long DAPPER_CLOCK_TIMEOUT_MS = 8000;
static const int DAPPER_MAX_INSTALLED = 32;

//The live site currently chains through Let's Encrypt YE to ISRG Root X2. This
//root comes from https://letsencrypt.org/certs/isrg-root-x2.pem and expires in
//2040. Keeping a CA here makes a changed catalog an authenticated repository
//update rather than something any network peer can impersonate.
static const char DAPPER_ISRG_ROOT_X2[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)PEM";

static void dapperServiceUi() {
    ftpService();
    radioService();
    ledService();
    drawDisplayFrame();
    delay(1);
}

static bool dapperEnsureDirectory(const char* path) {
    File existing = LittleFS.open(path);
    if (existing) {
        bool isDirectory = existing.isDirectory();
        existing.close();
        return isDirectory;
    }
    return LittleFS.mkdir(path);
}

static bool dapperEnsureStorage(String& error) {
    if (!dapperEnsureDirectory(DAPPER_STATE_DIR)) {
        error = "cannot create /.dapper";
        return false;
    }
    if (!dapperEnsureDirectory("/apps")) {
        error = "cannot create /apps";
        return false;
    }
    return true;
}

static String dapperInternalAppPath(const String& id) {
    return "/apps/" + id + ".dapp";
}

static String dapperSdAppPath(const String& id) {
    return "/sd/apps/" + id + ".dapp";
}

static bool dapperIsManagedAppPath(const String& id, const String& path) {
    return path == dapperInternalAppPath(id) || path == dapperSdAppPath(id);
}

static bool dapperPathIsSd(const String& path) {
    return path == "/sd" || path.startsWith("/sd/");
}

static bool dapperFileExists(const String& path) {
    RoutedPath r = routePath(path);
    if (r.isSd && !sdCardMounted) return false;
    ledPulseStorageRead(r.isSd);
    File file = r.fs->open(r.realPath, "r");
    bool ok = file && !file.isDirectory();
    if (file) file.close();
    return ok;
}

static bool dapperRemoveFile(const String& path) {
    RoutedPath r = routePath(path);
    if (r.isSd && !sdCardMounted) return false;
    ledPulseStorageWrite(r.isSd);
    return r.fs->remove(r.realPath);
}

static bool dapperRenameFile(const String& fromPath, const String& toPath) {
    RoutedPath from = routePath(fromPath);
    RoutedPath to = routePath(toPath);
    if (from.fs != to.fs || from.isSd != to.isSd) return false;
    if (from.isSd && !sdCardMounted) return false;
    ledPulseStorageWrite(from.isSd);
    return from.fs->rename(from.realPath, to.realPath);
}

static bool dapperEnsureAppDirectoryForTarget(const String& target, String& error) {
    RoutedPath r = routePath(target);
    if (r.isSd && !sdCardMounted) {
        error = "SD not mounted";
        return false;
    }

    ledPulseStorageRead(r.isSd);
    File appsDir = r.fs->open("/apps");
    if (appsDir && appsDir.isDirectory()) {
        appsDir.close();
        return true;
    }
    if (appsDir) appsDir.close();

    ledPulseStorageWrite(r.isSd);
    if (!r.fs->mkdir("/apps")) {
        error = r.isSd ? "cannot create /sd/apps" : "cannot create /apps";
        return false;
    }
    return true;
}

static bool dapperParseStableVersion(const String& value, int parts[3]) {
    int firstDot = value.indexOf('.');
    int secondDot = firstDot >= 0 ? value.indexOf('.', firstDot + 1) : -1;
    if (firstDot <= 0 || secondDot <= firstDot + 1 || secondDot >= (int)value.length() - 1) {
        return false;
    }
    String fields[3] = {
        value.substring(0, firstDot),
        value.substring(firstDot + 1, secondDot),
        value.substring(secondDot + 1),
    };
    for (int field = 0; field < 3; field++) {
        if (fields[field].length() == 0) return false;
        for (int i = 0; i < (int)fields[field].length(); i++) {
            if (!isDigit(fields[field][i])) return false;
        }
        parts[field] = fields[field].toInt();
    }
    return true;
}

static int dapperCompareVersions(const String& left, const String& right) {
    int a[3];
    int b[3];
    if (!dapperParseStableVersion(left, a) || !dapperParseStableVersion(right, b)) return 0;
    for (int i = 0; i < 3; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static bool dapperRuntimeCompatible(const String& minimum, const String& maximumExclusive) {
    int ignored[3];
    if (!dapperParseStableVersion(minimum, ignored) ||
        !dapperParseStableVersion(maximumExclusive, ignored)) {
        return false;
    }
    return dapperCompareVersions(DAPP_RUNTIME_VERSION, minimum) >= 0 &&
           dapperCompareVersions(DAPP_RUNTIME_VERSION, maximumExclusive) < 0;
}

static bool dapperValidId(const String& id) {
    if (id.length() < 1 || id.length() > 32) return false;
    for (int i = 0; i < (int)id.length(); i++) {
        char ch = id[i];
        if (!(isLowerCase(ch) || isDigit(ch) || (ch == '-' && i > 0))) return false;
    }
    return true;
}

static bool dapperValidSha256(const String& hash) {
    if (hash.length() != 64) return false;
    for (int i = 0; i < 64; i++) {
        char ch = hash[i];
        if (!isDigit(ch) && !(ch >= 'a' && ch <= 'f')) return false;
    }
    return true;
}

static String dapperShaHex(const unsigned char digest[32]) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    char output[65];
    for (int i = 0; i < 32; i++) {
        output[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0f];
        output[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0f];
    }
    output[64] = '\0';
    return String(output);
}

static bool dapperFetchToFile(const String& url, const char* destination,
                              size_t maximumBytes, size_t expectedBytes,
                              const String& expectedSha256, String& actualSha256,
                              String& error) {
    if (wifiIsConnected() != 1) {
        error = "Wi-Fi is not connected";
        return false;
    }
    if (!ntpEnsureClock(error, DAPPER_CLOCK_TIMEOUT_MS, dapperServiceUi)) return false;

    ledPulseNetwork();
    WiFiClientSecure* secureClient = new WiFiClientSecure();
    HTTPClient* http = new HTTPClient();
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(DAPPER_IO_BUFFER_BYTES, MALLOC_CAP_8BIT);
    if (!secureClient || !http || !buffer) {
        delete http;
        delete secureClient;
        if (buffer) heap_caps_free(buffer);
        error = "not enough heap for HTTPS download";
        return false;
    }

    secureClient->setCACert(DAPPER_ISRG_ROOT_X2);
    http->setConnectTimeout(10000);
    http->setTimeout(DAPPER_HTTP_IDLE_TIMEOUT_MS);
    if (!http->begin(*secureClient, url)) {
        error = "could not begin HTTPS request";
        delete http;
        delete secureClient;
        heap_caps_free(buffer);
        return false;
    }

    int status = http->GET();
    if (status != HTTP_CODE_OK) {
        error = "HTTP " + String(status);
        http->end();
        delete http;
        delete secureClient;
        heap_caps_free(buffer);
        return false;
    }
    int contentLength = http->getSize();
    if (contentLength > 0 && (size_t)contentLength > maximumBytes) {
        error = "response is larger than the allowed limit";
        http->end();
        delete http;
        delete secureClient;
        heap_caps_free(buffer);
        return false;
    }
    if (expectedBytes > 0 && contentLength > 0 && (size_t)contentLength != expectedBytes) {
        error = "Content-Length does not match the catalog";
        http->end();
        delete http;
        delete secureClient;
        heap_caps_free(buffer);
        return false;
    }

    ledPulseStorageWrite(false);
    LittleFS.remove(destination);
    File output = LittleFS.open(destination, "w");
    if (!output) {
        error = "cannot create temporary download";
        http->end();
        delete http;
        delete secureClient;
        heap_caps_free(buffer);
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    WiFiClient* stream = http->getStreamPtr();
    size_t total = 0;
    unsigned long lastActivity = millis();
    bool failed = false;

    while (http->connected() || stream->available()) {
        int available = stream->available();
        if (available <= 0) {
            if (contentLength >= 0 && total >= (size_t)contentLength) break;
            if (millis() - lastActivity > DAPPER_HTTP_IDLE_TIMEOUT_MS) {
                error = "download timed out";
                failed = true;
                break;
            }
            dapperServiceUi();
            continue;
        }

        size_t wanted = (size_t)available;
        if (wanted > DAPPER_IO_BUFFER_BYTES) wanted = DAPPER_IO_BUFFER_BYTES;
        int received = stream->read(buffer, wanted);
        ledPulseNetwork();
        if (received <= 0) {
            dapperServiceUi();
            continue;
        }
        total += (size_t)received;
        if (total > maximumBytes) {
            error = "download exceeded the allowed limit";
            failed = true;
            break;
        }
        if (output.write(buffer, (size_t)received) != (size_t)received) {
            error = "filesystem write failed";
            failed = true;
            break;
        }
        ledPulseStorageWrite(false);
        mbedtls_sha256_update(&sha, buffer, (size_t)received);
        lastActivity = millis();
        dapperServiceUi();
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    output.close();
    http->end();
    delete http;
    delete secureClient;
    heap_caps_free(buffer);
    actualSha256 = dapperShaHex(digest);

    if (!failed && contentLength >= 0 && total != (size_t)contentLength) {
        error = "download ended before Content-Length bytes arrived";
        failed = true;
    }
    if (!failed && expectedBytes > 0 && total != expectedBytes) {
        error = "downloaded size does not match the catalog";
        failed = true;
    }
    if (!failed && expectedSha256.length() > 0 && actualSha256 != expectedSha256) {
        error = "SHA-256 does not match the catalog";
        failed = true;
    }
    if (failed) {
        ledPulseError();
        LittleFS.remove(destination);
    }
    return !failed;
}

static bool dapperAtomicReplace(const char* temporary, const char* destination, String& error) {
    String backup = String(destination) + ".bak";
    ledPulseStorageWrite(false);
    LittleFS.remove(backup);
    bool hadDestination = LittleFS.exists(destination);
    if (hadDestination && !LittleFS.rename(destination, backup)) {
        error = "cannot back up " + String(destination);
        return false;
    }
    if (!LittleFS.rename(temporary, destination)) {
        if (hadDestination) LittleFS.rename(backup, destination);
        error = "cannot commit " + String(destination);
        return false;
    }
    if (hadDestination) LittleFS.remove(backup);
    return true;
}

static bool dapperCatalogHasBoard(JsonArrayConst boards) {
    for (JsonVariantConst value : boards) {
        const char* board = value.as<const char*>();
        if (board && String(board) == DOLL_BOARD_ID) return true;
    }
    return false;
}

static bool dapperSafeArtifactUrl(const String& url) {
    return url.length() > 0 && !url.startsWith("/") && url.indexOf("://") < 0 &&
           url.indexOf("..") < 0 && url.indexOf('\\') < 0;
}

static bool dapperParseCatalogRecord(const String& line, DapperRecord& record, String& error) {
    JsonDocument document;
    DeserializationError jsonError = deserializeJson(document, line);
    if (jsonError) {
        error = "invalid catalog JSON: " + String(jsonError.c_str());
        return false;
    }

    record = DapperRecord();
    record.packageFormat = document["package_format"] | 0;
    record.id = String((const char*)(document["id"] | ""));
    record.name = String((const char*)(document["name"] | ""));
    record.summary = String((const char*)(document["summary"] | ""));
    record.version = String((const char*)(document["version"] | ""));
    record.runtimeMin = String((const char*)(document["runtime_min"] | ""));
    record.runtimeMaxExclusive = String((const char*)(document["runtime_max_exclusive"] | ""));
    record.sha256 = String((const char*)(document["sha256"] | ""));
    record.url = String((const char*)(document["url"] | ""));
    record.size = document["size"] | 0;

    int versionParts[3];
    if (!dapperValidId(record.id) || record.name.length() == 0 ||
        !dapperParseStableVersion(record.version, versionParts) ||
        !dapperValidSha256(record.sha256) || record.size == 0 ||
        record.size > DAPPER_MAX_PACKAGE_BYTES || !dapperSafeArtifactUrl(record.url) ||
        !document["boards"].is<JsonArray>()) {
        error = "catalog record has invalid required fields";
        return false;
    }

    record.compatible = true;
    if (record.packageFormat != DAPP_PACKAGE_FORMAT) {
        record.compatible = false;
        record.incompatibility = "package format " + String(record.packageFormat);
    } else if (!dapperCatalogHasBoard(document["boards"].as<JsonArrayConst>())) {
        record.compatible = false;
        record.incompatibility = "not published for " + String(DOLL_BOARD_ID);
    } else if (!dapperRuntimeCompatible(record.runtimeMin, record.runtimeMaxExclusive)) {
        record.compatible = false;
        record.incompatibility = "requires AppRunner >=" + record.runtimeMin + " <" +
                                 record.runtimeMaxExclusive;
    }
    return true;
}

static bool dapperValidateCatalog(const char* path, int& recordCount, String& error) {
    ledPulseStorageRead(false);
    File catalog = LittleFS.open(path, "r");
    if (!catalog || catalog.isDirectory()) {
        if (catalog) catalog.close();
        error = "cannot open downloaded catalog";
        return false;
    }
    recordCount = 0;
    while (catalog.available()) {
        ledPulseStorageRead(false);
        String line = catalog.readStringUntil('\n');
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        line.trim();
        if (line.length() == 0) continue;
        if (line.length() > DAPPER_MAX_CATALOG_LINE) {
            catalog.close();
            error = "catalog line is too long";
            return false;
        }
        DapperRecord record;
        String parseError;
        if (!dapperParseCatalogRecord(line, record, parseError)) {
            catalog.close();
            error = "catalog line " + String(recordCount + 1) + ": " + parseError;
            return false;
        }
        recordCount++;
    }
    catalog.close();
    if (recordCount == 0) {
        error = "catalog contains no package records";
        return false;
    }
    return true;
}

static bool dapperRefreshCatalog(bool verbose, String& error) {
    if (!dapperEnsureStorage(error)) return false;
    if (verbose) outLine("Dapper: fetching repository metadata...", C_CYAN);

    String ignoredHash;
    if (!dapperFetchToFile(DAPPER_REPOSITORY_URL, DAPPER_REPO_PART_PATH,
                           DAPPER_MAX_REPO_BYTES, 0, "", ignoredHash, error)) {
        return false;
    }
    ledPulseStorageRead(false);
    File repoFile = LittleFS.open(DAPPER_REPO_PART_PATH, "r");
    JsonDocument repository;
    DeserializationError repoError = deserializeJson(repository, repoFile);
    repoFile.close();
    ledPulseStorageWrite(false);
    LittleFS.remove(DAPPER_REPO_PART_PATH);
    if (repoError) {
        error = "invalid repo.json: " + String(repoError.c_str());
        return false;
    }

    int repositoryFormat = repository["repository_format"] | 0;
    String repositoryId = String((const char*)(repository["id"] | ""));
    String canonicalUrl = String((const char*)(repository["canonical_url"] | ""));
    String catalogName = String((const char*)(repository["catalog"] | ""));
    if (repositoryFormat != 1 || repositoryId != DAPPER_REPOSITORY_ID ||
        canonicalUrl != DAPPER_REPOSITORY_BASE_URL || catalogName != "catalog-v1.ndjson") {
        error = "repository identity or format does not match Dapper's configuration";
        return false;
    }

    if (verbose) outLine("Dapper: fetching catalog...", C_CYAN);
    if (!dapperFetchToFile(canonicalUrl + catalogName, DAPPER_CATALOG_PART_PATH,
                           DAPPER_MAX_CATALOG_BYTES, 0, "", ignoredHash, error)) {
        return false;
    }
    int records = 0;
    if (!dapperValidateCatalog(DAPPER_CATALOG_PART_PATH, records, error)) {
        LittleFS.remove(DAPPER_CATALOG_PART_PATH);
        return false;
    }
    if (!dapperAtomicReplace(DAPPER_CATALOG_PART_PATH, DAPPER_CATALOG_PATH, error)) {
        return false;
    }
    if (verbose) outLine("Dapper: catalog ready (" + String(records) + " artifacts)", C_GREEN);
    return true;
}

static bool dapperPrepareCatalog() {
    String error;
    if (dapperRefreshCatalog(false, error)) return true;
    if (LittleFS.exists(DAPPER_CATALOG_PATH)) {
        outLine("Dapper: refresh failed; using cached catalog", C_YELLOW);
        outLine("  " + error, C_YELLOW);
        return true;
    }
    outLine("Dapper: " + error, C_RED);
    return false;
}

static bool dapperFindBest(const String& id, const String& exactVersion,
                           DapperRecord& best, String& failure) {
    File catalog = LittleFS.open(DAPPER_CATALOG_PATH, "r");
    if (!catalog) {
        failure = "catalog is unavailable";
        return false;
    }
    bool foundId = false;
    bool foundRequestedVersion = false;
    bool foundCompatible = false;
    String firstIncompatibility;
    while (catalog.available()) {
        String line = catalog.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        DapperRecord candidate;
        String parseError;
        if (!dapperParseCatalogRecord(line, candidate, parseError)) continue;
        if (candidate.id != id) continue;
        foundId = true;
        if (exactVersion.length() > 0 && candidate.version != exactVersion) continue;
        foundRequestedVersion = true;
        if (!candidate.compatible) {
            if (firstIncompatibility.length() == 0) firstIncompatibility = candidate.incompatibility;
            continue;
        }
        if (!foundCompatible || dapperCompareVersions(candidate.version, best.version) > 0) {
            best = candidate;
            foundCompatible = true;
        }
    }
    catalog.close();
    if (foundCompatible) return true;
    if (!foundId) failure = "package not found: " + id;
    else if (exactVersion.length() > 0 && !foundRequestedVersion) {
        failure = id + " version " + exactVersion + " was not found";
    } else if (firstIncompatibility.length() > 0) {
        failure = id + " is incompatible: " + firstIncompatibility;
    } else failure = "no compatible stable release found for " + id;
    return false;
}

static bool dapperParseInstalledLine(const String& line, DapperInstalled& installed) {
    JsonDocument document;
    if (deserializeJson(document, line)) return false;
    installed = DapperInstalled();
    installed.repository = String((const char*)(document["repository"] | ""));
    installed.installedPath = String((const char*)(document["installed_path"] | ""));
    installed.record.packageFormat = document["package_format"] | 0;
    installed.record.id = String((const char*)(document["id"] | ""));
    installed.record.name = String((const char*)(document["name"] | ""));
    installed.record.summary = String((const char*)(document["summary"] | ""));
    installed.record.version = String((const char*)(document["version"] | ""));
    installed.record.runtimeMin = String((const char*)(document["runtime_min"] | ""));
    installed.record.runtimeMaxExclusive = String((const char*)(document["runtime_max_exclusive"] | ""));
    installed.record.sha256 = String((const char*)(document["sha256"] | ""));
    installed.record.url = String((const char*)(document["url"] | ""));
    installed.record.size = document["size"] | 0;
    return installed.repository == DAPPER_REPOSITORY_ID && dapperValidId(installed.record.id) &&
           dapperValidSha256(installed.record.sha256) && installed.installedPath.length() > 0;
}

static bool dapperFindInstalled(const String& id, DapperInstalled& installed) {
    File registry = LittleFS.open(DAPPER_INSTALLED_PATH, "r");
    if (!registry) return false;
    while (registry.available()) {
        String line = registry.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        DapperInstalled candidate;
        if (dapperParseInstalledLine(line, candidate) && candidate.record.id == id) {
            installed = candidate;
            registry.close();
            return true;
        }
    }
    registry.close();
    return false;
}

static bool dapperRewriteInstalled(const String& removeId, const DapperRecord* appendRecord,
                                    const String& installedPath, String& error) {
    LittleFS.remove(DAPPER_INSTALLED_PART_PATH);
    File output = LittleFS.open(DAPPER_INSTALLED_PART_PATH, "w");
    if (!output) {
        error = "cannot create installed-package registry update";
        return false;
    }
    File current = LittleFS.open(DAPPER_INSTALLED_PATH, "r");
    if (current) {
        while (current.available()) {
            String line = current.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            DapperInstalled existing;
            if (!dapperParseInstalledLine(line, existing)) {
                current.close();
                output.close();
                LittleFS.remove(DAPPER_INSTALLED_PART_PATH);
                error = "installed-package registry is corrupt; run dapper doctor";
                return false;
            }
            if (existing.record.id != removeId) output.println(line);
        }
        current.close();
    }
    if (appendRecord) {
        JsonDocument document;
        document["repository"] = DAPPER_REPOSITORY_ID;
        document["package_format"] = appendRecord->packageFormat;
        document["id"] = appendRecord->id;
        document["name"] = appendRecord->name;
        document["summary"] = appendRecord->summary;
        document["version"] = appendRecord->version;
        document["runtime_min"] = appendRecord->runtimeMin;
        document["runtime_max_exclusive"] = appendRecord->runtimeMaxExclusive;
        document["size"] = appendRecord->size;
        document["sha256"] = appendRecord->sha256;
        document["url"] = appendRecord->url;
        document["installed_path"] = installedPath;
        serializeJson(document, output);
        output.println();
    }
    output.close();
    return dapperAtomicReplace(DAPPER_INSTALLED_PART_PATH, DAPPER_INSTALLED_PATH, error);
}

static bool dapperHeaderHasBoard(const String& boards) {
    int start = 0;
    while (start <= (int)boards.length()) {
        int comma = boards.indexOf(',', start);
        if (comma < 0) comma = boards.length();
        String board = boards.substring(start, comma);
        board.trim();
        if (board == DOLL_BOARD_ID) return true;
        start = comma + 1;
    }
    return false;
}

static bool dapperValidateDownloadedPackage(const DapperRecord& record, String& error) {
    File package = LittleFS.open(DAPPER_PACKAGE_PART_PATH, "r");
    if (!package) {
        error = "cannot reopen downloaded package";
        return false;
    }
    String format;
    String id;
    String version;
    String boards;
    String runtime;
    int headerLines = 0;
    while (package.available() && headerLines < 32 && package.position() < 2048) {
        String line = package.readStringUntil('\n');
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        headerLines++;
        String trimmed = line;
        trimmed.trim();
        if (trimmed.length() > 0 && !trimmed.startsWith("#") && !trimmed.startsWith("//")) break;
        if (!trimmed.startsWith("# @")) continue;
        String fieldAndValue = trimmed.substring(3);
        int space = fieldAndValue.indexOf(' ');
        if (space <= 0) continue;
        String field = fieldAndValue.substring(0, space);
        String value = fieldAndValue.substring(space + 1);
        value.trim();
        if (field == "dapp-format") format = value;
        else if (field == "id") id = value;
        else if (field == "version") version = value;
        else if (field == "boards") boards = value;
        else if (field == "runtime") runtime = value;
    }
    package.close();
    String expectedRuntime = ">=" + record.runtimeMin + " <" + record.runtimeMaxExclusive;
    if (format != String(DAPP_PACKAGE_FORMAT) || id != record.id || version != record.version ||
        !dapperHeaderHasBoard(boards) || runtime != expectedRuntime) {
        error = "downloaded metadata does not match the catalog";
        return false;
    }

    package = LittleFS.open(DAPPER_PACKAGE_PART_PATH, "r");
    int lineCount = 0;
    while (package.available()) {
        package.readStringUntil('\n');
        lineCount++;
        if (lineCount > DAPP_MAX_LINES) break;
    }
    package.close();
    if (lineCount > DAPP_MAX_LINES) {
        error = "package exceeds this board's AppRunner line limit";
        return false;
    }
    return true;
}

bool dapperFetchPackageForEdit(const String& request, const String& targetPreference,
                               String& sourcePath, String& suggestedSavePath,
                               String& loadedLabel, String& error) {
    String id = request;
    String version;
    int at = request.indexOf('@');
    if (at >= 0) {
        id = request.substring(0, at);
        version = request.substring(at + 1);
    }
    int versionParts[3];
    if (!dapperValidId(id) || (version.length() > 0 && !dapperParseStableVersion(version, versionParts))) {
        error = "invalid package ID or version";
        return false;
    }

    String preference = targetPreference;
    preference.toLowerCase();
    if (preference.length() > 0 && preference != "internal" && preference != "flash" && preference != "sd") {
        error = "unknown edit target preference: " + targetPreference;
        return false;
    }

    if (!dapperPrepareCatalog()) {
        error = "catalog is unavailable";
        return false;
    }

    DapperRecord record;
    String failure;
    if (!dapperFindBest(id, version, record, failure)) {
        error = failure;
        return false;
    }

    if (!dapperEnsureStorage(error)) {
        return false;
    }

    bool preferSd = preference == "sd" || (preference.length() == 0 && sdCardMounted);
    if (preferSd && !sdCardMounted) {
        error = "SD not mounted";
        return false;
    }
    suggestedSavePath = preferSd ? dapperSdAppPath(record.id) : dapperInternalAppPath(record.id);
    if (!dapperEnsureAppDirectoryForTarget(suggestedSavePath, error)) {
        return false;
    }

    outLine("Dapper: loading " + record.id + " " + record.version + " into editor...", C_CYAN);
    String actualHash;
    if (!dapperFetchToFile(String(DAPPER_REPOSITORY_BASE_URL) + record.url,
                           DAPPER_PACKAGE_PART_PATH, DAPPER_MAX_PACKAGE_BYTES,
                           record.size, record.sha256, actualHash, error)) {
        return false;
    }
    if (!dapperValidateDownloadedPackage(record, error)) {
        ledPulseStorageWrite(false);
        LittleFS.remove(DAPPER_PACKAGE_PART_PATH);
        return false;
    }

    sourcePath = DAPPER_PACKAGE_PART_PATH;
    loadedLabel = record.id + " " + record.version;
    return true;
}

static bool dapperHashFile(const String& path, String& hash, size_t& size) {
    RoutedPath r = routePath(path);
    if (r.isSd && !sdCardMounted) return false;
    ledPulseStorageRead(r.isSd);
    File file = r.fs->open(r.realPath, "r");
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(DAPPER_IO_BUFFER_BYTES, MALLOC_CAP_8BIT);
    if (!buffer) {
        mbedtls_sha256_free(&sha);
        file.close();
        return false;
    }
    size = 0;
    while (file.available()) {
        int received = file.read(buffer, DAPPER_IO_BUFFER_BYTES);
        ledPulseStorageRead(r.isSd);
        if (received <= 0) break;
        size += (size_t)received;
        mbedtls_sha256_update(&sha, buffer, (size_t)received);
    }
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    heap_caps_free(buffer);
    file.close();
    hash = dapperShaHex(digest);
    return true;
}

static bool dapperCopyDownloadedPackageToTargetPart(const String& targetPart, String& error) {
    File input = LittleFS.open(DAPPER_PACKAGE_PART_PATH, "r");
    if (!input || input.isDirectory()) {
        if (input) input.close();
        error = "cannot reopen verified download";
        return false;
    }

    RoutedPath part = routePath(targetPart);
    if (part.isSd && !sdCardMounted) {
        input.close();
        error = "SD not mounted";
        return false;
    }
    part.fs->remove(part.realPath);
    ledPulseStorageWrite(part.isSd);
    File output = part.fs->open(part.realPath, "w");
    if (!output) {
        input.close();
        error = "cannot create package staging file";
        return false;
    }

    uint8_t* buffer = (uint8_t*)heap_caps_malloc(DAPPER_IO_BUFFER_BYTES, MALLOC_CAP_8BIT);
    if (!buffer) {
        input.close();
        output.close();
        dapperRemoveFile(targetPart);
        error = "not enough heap for package copy";
        return false;
    }
    while (input.available()) {
        int received = input.read(buffer, DAPPER_IO_BUFFER_BYTES);
        ledPulseStorageRead(false);
        if (received <= 0) break;
        if (output.write(buffer, (size_t)received) != (size_t)received) {
            heap_caps_free(buffer);
            input.close();
            output.close();
            dapperRemoveFile(targetPart);
            error = "filesystem write failed";
            return false;
        }
        ledPulseStorageWrite(part.isSd);
        dapperServiceUi();
    }

    heap_caps_free(buffer);
    input.close();
    output.close();
    ledPulseStorageWrite(false);
    LittleFS.remove(DAPPER_PACKAGE_PART_PATH);
    return true;
}

static bool dapperInstallRecord(const DapperRecord& record, bool force, const String& target) {
    String error;
    if (!dapperEnsureStorage(error)) {
        outLine("Dapper: " + error, C_RED);
        return false;
    }
    if (!dapperIsManagedAppPath(record.id, target)) {
        outLine("Dapper: refusing unsupported install path: " + target, C_RED);
        return false;
    }
    if (!dapperEnsureAppDirectoryForTarget(target, error)) {
        outLine("Dapper: " + error, C_RED);
        return false;
    }

    DapperInstalled installed;
    bool managed = dapperFindInstalled(record.id, installed);
    if (dapperFileExists(target) && (!managed || installed.installedPath != target) && !force) {
        outLine("Dapper: " + target + " is unmanaged; use --force to take ownership", C_RED);
        return false;
    }
    if (managed && !dapperIsManagedAppPath(record.id, installed.installedPath)) {
        outLine("Dapper: installed path is not supported by this client: " + installed.installedPath, C_RED);
        return false;
    }
    if (managed && installed.record.version == record.version &&
        installed.record.sha256 == record.sha256 && installed.installedPath == target &&
        dapperFileExists(target)) {
        outLine(record.id + " " + record.version + " is already installed at " + target, C_GREEN);
        return true;
    }

    outLine("Dapper: downloading " + record.id + " " + record.version + "...", C_CYAN);
    String actualHash;
    if (!dapperFetchToFile(String(DAPPER_REPOSITORY_BASE_URL) + record.url,
                           DAPPER_PACKAGE_PART_PATH, DAPPER_MAX_PACKAGE_BYTES,
                           record.size, record.sha256, actualHash, error)) {
        outLine("Dapper: " + error, C_RED);
        return false;
    }
    if (!dapperValidateDownloadedPackage(record, error)) {
        ledPulseStorageWrite(false);
        LittleFS.remove(DAPPER_PACKAGE_PART_PATH);
        outLine("Dapper: " + error, C_RED);
        return false;
    }

    String targetPart = target + ".part.dsys";
    String targetBackup = target + ".bak.dsys";
    dapperRemoveFile(targetPart);
    dapperRemoveFile(targetBackup);
    if (!dapperCopyDownloadedPackageToTargetPart(targetPart, error)) {
        ledPulseStorageWrite(false);
        LittleFS.remove(DAPPER_PACKAGE_PART_PATH);
        outLine("Dapper: " + error, C_RED);
        return false;
    }

    bool hadTarget = dapperFileExists(target);
    if (hadTarget && !dapperRenameFile(target, targetBackup)) {
        dapperRemoveFile(targetPart);
        outLine("Dapper: cannot back up the installed app", C_RED);
        return false;
    }
    if (!dapperRenameFile(targetPart, target)) {
        if (hadTarget) dapperRenameFile(targetBackup, target);
        outLine("Dapper: cannot commit the downloaded app", C_RED);
        return false;
    }
    if (!dapperRewriteInstalled(record.id, &record, target, error)) {
        dapperRemoveFile(target);
        if (hadTarget) dapperRenameFile(targetBackup, target);
        outLine("Dapper: " + error + "; previous app restored", C_RED);
        return false;
    }
    dapperRemoveFile(targetBackup);
    if (managed && installed.installedPath != target && dapperFileExists(installed.installedPath)) {
        if (!dapperRemoveFile(installed.installedPath)) {
            outLine("Dapper: installed at " + target + ", but old copy remains at " +
                    installed.installedPath, C_YELLOW);
        }
    }
    outLine("Installed " + record.id + " " + record.version + " at " + target, C_GREEN);
    return true;
}

static void dapperCommandSearch(const String parts[], int partCount) {
    if (!dapperPrepareCatalog()) return;
    String query;
    for (int i = 2; i < partCount; i++) {
        if (query.length() > 0) query += " ";
        query += parts[i];
    }
    query.toLowerCase();
    File catalog = LittleFS.open(DAPPER_CATALOG_PATH, "r");
    int matches = 0;
    while (catalog.available()) {
        String line = catalog.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        DapperRecord record;
        String error;
        if (!dapperParseCatalogRecord(line, record, error) || !record.compatible) continue;
        String haystack = record.id + " " + record.name + " " + record.summary;
        haystack.toLowerCase();
        if (query.length() > 0 && haystack.indexOf(query) < 0) continue;
        outLine(record.id + " " + record.version + " - " + record.name, C_GREEN);
        if (record.summary.length() > 0) outLine("  " + record.summary);
        matches++;
    }
    catalog.close();
    if (matches == 0) outLine("Dapper: no compatible packages found", C_YELLOW);
}

static void dapperCommandInfo(const String& id) {
    if (!dapperValidId(id)) {
        outLine("Dapper: invalid package ID", C_RED);
        return;
    }
    if (!dapperPrepareCatalog()) return;
    DapperInstalled installed;
    if (dapperFindInstalled(id, installed)) {
        outLine("Installed: " + installed.record.version + " at " + installed.installedPath, C_GREEN);
    } else outLine("Installed: no");

    File catalog = LittleFS.open(DAPPER_CATALOG_PATH, "r");
    int matches = 0;
    while (catalog.available()) {
        String line = catalog.readStringUntil('\n');
        line.trim();
        DapperRecord record;
        String error;
        if (line.length() == 0 || !dapperParseCatalogRecord(line, record, error) || record.id != id) continue;
        outLine(record.id + " " + record.version + " - " + record.name,
                record.compatible ? C_GREEN : C_YELLOW);
        outLine(record.compatible ? "  compatible" : "  incompatible: " + record.incompatibility,
                record.compatible ? C_WHITE : C_YELLOW);
        if (record.summary.length() > 0) outLine("  " + record.summary);
        matches++;
    }
    catalog.close();
    if (matches == 0) outLine("Dapper: package not found: " + id, C_RED);
}

static String dapperPromptInstallTarget(const DapperRecord& record, const String& currentPath) {
    if (!sdCardMounted) {
        outLine("Dapper: SD not mounted; using internal flash", C_YELLOW);
        return dapperInternalAppPath(record.id);
    }

    String defaultChoice = dapperPathIsSd(currentPath) ? "s" : "i";
    outLine("Dapper: install " + record.id + " " + record.version + " where?", C_CYAN);
    outLine("  [i] internal flash: " + dapperInternalAppPath(record.id));
    outLine("  [s] SD card:        " + dapperSdAppPath(record.id));
    outLine("Press Enter for " + String(defaultChoice == "s" ? "SD card" : "internal flash") + ".");

    String answer = "";
    commandCursorPos = 0;
    const String prompt = "dapper install> ";
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(prompt);
    }
    setActiveInput(prompt, answer, false);

    while (true) {
        LineInputResult r = readLineEditedInput(answer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(answer);
        }
        setActiveInput(prompt, answer, false);
        dapperServiceUi();

        if (r == LINE_SUBMITTED) {
            String choice = answer;
            choice.trim();
            choice.toLowerCase();
            if (choice.length() == 0) choice = defaultChoice;
            outLine(prompt + choice, C_CYAN);
            commandCursorPos = 0;
            setActiveInput(shellPrompt(), "", false);

            if (choice == "i" || choice == "internal" || choice == "flash") {
                return dapperInternalAppPath(record.id);
            }
            if (choice == "s" || choice == "sd" || choice == "card") {
                return dapperSdAppPath(record.id);
            }

            outLine("Dapper: choose i/internal or s/sd", C_RED);
            answer = "";
            commandCursorPos = 0;
            if (telnetClient && telnetClient.connected()) {
                telnetClient.print(prompt);
            }
            setActiveInput(prompt, answer, false);
        }

        delay(1);
    }
}

static void dapperCommandInstall(const String parts[], int partCount) {
    if (partCount < 3) {
        outLine("Usage: dapper install <id>[@version] [--force] [--internal|--sd]");
        return;
    }
    String request = parts[2];
    String id = request;
    String version;
    int at = request.indexOf('@');
    if (at >= 0) {
        id = request.substring(0, at);
        version = request.substring(at + 1);
    }
    bool force = false;
    String requestedTarget;
    for (int i = 3; i < partCount; i++) {
        if (parts[i] == "--force") force = true;
        else if (parts[i] == "--internal" || parts[i] == "--flash") {
            if (requestedTarget.length() > 0) {
                outLine("Dapper: choose only one install location", C_RED);
                return;
            }
            requestedTarget = dapperInternalAppPath(id);
        } else if (parts[i] == "--sd") {
            if (requestedTarget.length() > 0) {
                outLine("Dapper: choose only one install location", C_RED);
                return;
            }
            requestedTarget = dapperSdAppPath(id);
        }
        else {
            outLine("Dapper: unknown install option: " + parts[i], C_RED);
            return;
        }
    }
    int versionParts[3];
    if (!dapperValidId(id) || (version.length() > 0 && !dapperParseStableVersion(version, versionParts))) {
        outLine("Dapper: invalid package ID or version", C_RED);
        return;
    }
    if (!dapperPrepareCatalog()) return;
    DapperRecord record;
    String failure;
    if (!dapperFindBest(id, version, record, failure)) {
        outLine("Dapper: " + failure, C_RED);
        return;
    }

    DapperInstalled installed;
    bool managed = dapperFindInstalled(record.id, installed);
    String target = requestedTarget;
    if (target.length() == 0) {
        target = dapperPromptInstallTarget(record, managed ? installed.installedPath : "");
    }
    dapperInstallRecord(record, force, target);
}

static bool dapperUpdateOne(const String& id) {
    DapperInstalled installed;
    if (!dapperFindInstalled(id, installed)) {
        outLine("Dapper: not installed: " + id, C_RED);
        return false;
    }
    DapperRecord available;
    String failure;
    if (!dapperFindBest(id, "", available, failure)) {
        outLine("Dapper: " + failure, C_RED);
        return false;
    }
    if (dapperCompareVersions(available.version, installed.record.version) <= 0) {
        outLine(id + " is current at " + installed.record.version, C_GREEN);
        return true;
    }
    return dapperInstallRecord(available, false, installed.installedPath);
}

static void dapperCommandUpdate(const String parts[], int partCount) {
    if (!dapperPrepareCatalog()) return;
    if (partCount >= 3 && parts[2] != "--all") {
        dapperUpdateOne(parts[2]);
        return;
    }
    String ids[DAPPER_MAX_INSTALLED];
    int count = 0;
    File registry = LittleFS.open(DAPPER_INSTALLED_PATH, "r");
    if (registry) {
        while (registry.available() && count < DAPPER_MAX_INSTALLED) {
            String line = registry.readStringUntil('\n');
            line.trim();
            DapperInstalled installed;
            if (line.length() > 0 && dapperParseInstalledLine(line, installed)) {
                ids[count++] = installed.record.id;
            }
        }
        registry.close();
    }
    if (count == 0) {
        outLine("Dapper: no managed packages are installed", C_YELLOW);
        return;
    }
    for (int i = 0; i < count; i++) dapperUpdateOne(ids[i]);
}

static void dapperCommandRemove(const String& id) {
    DapperInstalled installed;
    if (!dapperFindInstalled(id, installed)) {
        outLine("Dapper: not installed: " + id, C_RED);
        return;
    }
    if (!dapperIsManagedAppPath(id, installed.installedPath)) {
        outLine("Dapper: refusing unexpected installed path " + installed.installedPath, C_RED);
        return;
    }
    String backupPath = installed.installedPath + ".bak.dsys";
    dapperRemoveFile(backupPath);
    bool hadFile = dapperFileExists(installed.installedPath);
    if (hadFile && !dapperRenameFile(installed.installedPath, backupPath)) {
        outLine("Dapper: cannot back up installed app", C_RED);
        return;
    }
    String error;
    if (!dapperRewriteInstalled(id, nullptr, "", error)) {
        if (hadFile) dapperRenameFile(backupPath, installed.installedPath);
        outLine("Dapper: " + error + "; app restored", C_RED);
        return;
    }
    dapperRemoveFile(backupPath);
    outLine("Removed " + id + " (save data kept)", C_GREEN);
}

static void dapperCommandDoctor() {
    File registry = LittleFS.open(DAPPER_INSTALLED_PATH, "r");
    if (!registry) {
        outLine("Dapper: no managed packages are installed", C_YELLOW);
        return;
    }
    int checked = 0;
    int problems = 0;
    while (registry.available()) {
        String line = registry.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        DapperInstalled installed;
        if (!dapperParseInstalledLine(line, installed)) {
            outLine("Dapper: corrupt installed-package registry entry", C_RED);
            problems++;
            continue;
        }
        String hash;
        size_t size = 0;
        if (!dapperHashFile(installed.installedPath, hash, size)) {
            outLine(installed.record.id + ": missing", C_RED);
            problems++;
        } else if (hash != installed.record.sha256 || size != installed.record.size) {
            outLine(installed.record.id + ": modified or corrupt", C_RED);
            problems++;
        } else if (!dapperRuntimeCompatible(installed.record.runtimeMin,
                                             installed.record.runtimeMaxExclusive)) {
            outLine(installed.record.id + ": incompatible with this AppRunner", C_RED);
            problems++;
        } else outLine(installed.record.id + " " + installed.record.version + ": OK", C_GREEN);
        checked++;
    }
    registry.close();
    outLine("Dapper doctor: " + String(checked) + " checked, " + String(problems) + " problems",
            problems == 0 ? C_GREEN : C_YELLOW);
}

static void dapperPrintHelp() {
    outLine("Dapper - DOLL-OS .dapp package manager", C_CYAN);
    outLine("  dapper runtime");
    outLine("  dapper refresh");
    outLine("  dapper search [text]");
    outLine("  dapper info <id>");
    outLine("  dapper install <id>[@version] [--force] [--internal|--sd]");
    outLine("  dapper update [id|--all]");
    outLine("  dapper remove <id>");
    outLine("  dapper doctor");
}

void handleDapperCommand(const String parts[], int partCount) {
    if (partCount < 2 || parts[1] == "help") {
        dapperPrintHelp();
        return;
    }
    String action = parts[1];
    action.toLowerCase();
    if (action == "runtime") {
        outLine("Board: " + String(DOLL_BOARD_ID));
        outLine("AppRunner: " + String(DAPP_RUNTIME_VERSION));
        outLine("Package format: " + String(DAPP_PACKAGE_FORMAT));
        outLine("Repository: " + String(DAPPER_REPOSITORY_URL));
    } else if (action == "refresh") {
        String error;
        if (!dapperRefreshCatalog(true, error)) outLine("Dapper: " + error, C_RED);
    } else if (action == "search") {
        dapperCommandSearch(parts, partCount);
    } else if (action == "info") {
        if (partCount < 3) outLine("Usage: dapper info <id>");
        else dapperCommandInfo(parts[2]);
    } else if (action == "install") {
        dapperCommandInstall(parts, partCount);
    } else if (action == "update") {
        dapperCommandUpdate(parts, partCount);
    } else if (action == "remove") {
        if (partCount < 3) outLine("Usage: dapper remove <id>");
        else dapperCommandRemove(parts[2]);
    } else if (action == "doctor") {
        dapperCommandDoctor();
    } else {
        outLine("Dapper: unknown action: " + parts[1], C_RED);
        dapperPrintHelp();
    }
}
