//   Storage.ino
//   mounts internal (LittleFS) and SD storage, and provides the filesystem
//   commands. Ported from DOLL-OS's storage.ino -- the path-resolution/routing
//   logic is display-agnostic and carries over unchanged. What changed is the SD
//   transport: DOLL-OS's M5Cardputer wires its SD card over SPI, but this board's
//   SD slot is wired to the ESP32-S3's dedicated SDMMC peripheral (4-bit bus), so
//   this uses the SD_MMC library instead of SD/SPI. Pins come from config.h,
//   confirmed against Freenove's own example sketches for this board family.
#include <LittleFS.h>
#include <FS.h>
#include <SD_MMC.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>

static bool internalStorageInitialized = false;

//widens (or restores) the task watchdog timeout. Used instead of disableCore0WDT()/
//enableCore0WDT() around SD_MMC.begin() below -- those legacy per-core shims don't
//correctly re-subscribe the idle tasks on this IDF5-based core, which left the
//watchdog itself in a broken state and caused a *delayed* panic ~20s later instead of
//preventing one. Reconfiguring the timeout doesn't touch task subscriptions at all.
static void setTaskWdtTimeout(uint32_t timeoutMs) {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = timeoutMs,
        .idle_core_mask = 0x3,   //both cores' idle tasks
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdtConfig);
#else
    esp_task_wdt_init(timeoutMs / 1000, true);
#endif
}

bool ensureSystemConfDirectory() {
    const char* dirs[] = { "/system", "/system/conf" };
    for (const char* dirPath : dirs) {
        ledPulseStorageRead(false);
        File dir = LittleFS.open(dirPath);
        if (dir && dir.isDirectory()) {
            dir.close();
            continue;
        }
        if (dir) {
            dir.close();
        }
        ledPulseStorageWrite(false);
        if (!LittleFS.mkdir(dirPath)) {
            return false;
        }
    }
    return true;
}

//Mount internal storage before Wi-Fi starts so saved credentials are available during
//the boot connection pass. initStorage() calls this again later, so keep it idempotent.
void initInternalStorage() {
    if (internalStorageInitialized) {
        return;
    }

    //begin(true) asks esp_littlefs to auto-format when the mount fails, which covers a
    //blank first-boot partition. But a partition left half-written -- the "Corrupted
    //dir pair at {0x0, 0x1}" state -- isn't reliably recovered by that path, and DOLL-OS
    //then boots with settings storage dead (no saved network list -> falls back to the
    //config.h default SSID and can never reconnect). So on failure, force an explicit
    //format + clean remount; settings reset to defaults but storage lives again.
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS: mount failed; forcing format...");
        ledPulseError();
        if (LittleFS.format() && LittleFS.begin(false)) {
            ledPulseStorageWrite(false);
            outLine("LittleFS: was corrupt -- reformatted (settings reset)", C_YELLOW);
        } else {
            ledPulseError();
            outLine("LittleFS: mount failed -- settings won't persist", C_RED);
        }
    }
    ensureSystemConfDirectory();
    ensureDefaultAliases();
    internalStorageInitialized = true;
}

//mounts internal storage and the SD card, called once from setup()
void initStorage() {
    initInternalStorage();

    //with no card inserted, the SD_MMC driver's internal retry loop (sdmmc_init_ocr) can run
    //long enough without yielding that the default ~5s watchdog timeout trips mid-retry --
    //widen it just for this call so a missing card fails cleanly instead of resetting
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN, SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
    setTaskWdtTimeout(20000);
    sdCardMounted = SD_MMC.begin("/sdcard", false, false);
    ledSetSdMounted(sdCardMounted);
    setTaskWdtTimeout(5000);
    if (!sdCardMounted) {
        Serial.println("SD: not detected");
    }
}

//lists one directory of a mounted filesystem into the terminal.
//showSdMount adds a synthetic "sd" entry, used when listing flash root so the mount point is discoverable
void listDirectory(fs::FS& fs, const String& path, bool showSdMount, bool isSd) {
    ledPulseStorageRead(isSd);
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        if (isSd && path == "/" && sdCardMounted) {
            File apps = fs.open("/apps");
            if (apps && apps.isDirectory()) {
                outLine("  [DIR]  apps");
                apps.close();
                return;
            }
            if (apps) apps.close();
        }
        outLine("ls: " + path + " not found", C_RED);
        return;
    }

    File entry = dir.openNextFile();
    int entryCount = 0;
    while (entry) {
        ledPulseStorageRead(isSd);
        String line = entry.isDirectory() ? "  [DIR]  " : ("  " + String(entry.size()) + "b  ");
        line += entry.name();
        outLine(line);
        entryCount++;
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (showSdMount) {
        outLine("  [DIR]  sd");
        entryCount++;
    }

    if (entryCount == 0) {
        outLine("(empty)");
    }
}

//routes an absolute path in the unified namespace to the physical filesystem that owns it.
//paths at or under SD_MOUNT map onto the SD card with the mount prefix stripped; everything else is LittleFS
RoutedPath routePath(const String& resolvedPath) {
    bool onSd = (resolvedPath == SD_MOUNT) || resolvedPath.startsWith(SD_MOUNT + "/");
    if (onSd) {
        String realPath = resolvedPath.substring(SD_MOUNT.length());
        if (realPath.length() == 0) realPath = "/";
        return { &SD_MMC, realPath, true };
    }
    return { &LittleFS, resolvedPath, false };
}

//collapses "inputPath" (relative or absolute) against cwd into a clean absolute path in the
//unified namespace, resolving "." and ".." segments
String resolvePath(const String& cwd, const String& inputPath) {
    String combined = (inputPath.length() > 0 && inputPath[0] == '/')
        ? inputPath
        : cwd + "/" + inputPath;

    String stack[16];
    int depth = 0;

    int start = 0;
    while (start < combined.length()) {
        while (start < combined.length() && combined[start] == '/') start++;
        int end = combined.indexOf('/', start);
        if (end == -1) end = combined.length();
        String segment = combined.substring(start, end);

        if (segment.length() == 0 || segment == ".") {
            //skip
        } else if (segment == "..") {
            if (depth > 0) depth--;
        } else if (depth < 16) {
            stack[depth++] = segment;
        }
        start = end;
    }

    String result = "/";
    for (int i = 0; i < depth; i++) {
        result += stack[i];
        if (i < depth - 1) result += "/";
    }
    return result;
}

//true if resolvedPath is a real, openable directory once routed to its physical filesystem
bool directoryExists(const String& resolvedPath) {
    if (resolvedPath == SD_MOUNT) {
        return sdCardMounted;
    }
    RoutedPath r = routePath(resolvedPath);
    if (r.isSd && !sdCardMounted) {
        return false;
    }
    ledPulseStorageRead(r.isSd);
    File dir = r.fs->open(r.realPath);
    bool ok = dir && dir.isDirectory();
    if (dir) dir.close();
    return ok;
}

static bool pathIsProtectedRoot(const String& resolvedPath) {
    return resolvedPath == "/" || resolvedPath == SD_MOUNT;
}

static String parentPathOf(const String& resolvedPath) {
    if (resolvedPath == "/" || resolvedPath.length() == 0) {
        return "/";
    }
    int slash = resolvedPath.lastIndexOf('/');
    if (slash <= 0) {
        return "/";
    }
    return resolvedPath.substring(0, slash);
}

static String baseNameOf(const String& resolvedPath) {
    if (resolvedPath == "/" || resolvedPath.length() == 0) {
        return "";
    }
    int slash = resolvedPath.lastIndexOf('/');
    if (slash < 0) {
        return resolvedPath;
    }
    return resolvedPath.substring(slash + 1);
}

static String childPathOf(const String& parentResolved, bool parentIsSd, const String& childName) {
    if (childName.startsWith("/")) {
        return parentIsSd ? SD_MOUNT + childName : childName;
    }
    if (parentResolved == "/") {
        return "/" + childName;
    }
    return parentResolved + "/" + childName;
}

static bool ensureMountedForPath(const String& commandName, const RoutedPath& r) {
    if (r.isSd && !sdCardMounted) {
        outLine(commandName + ": SD not mounted (insert card and reboot)", C_RED);
        return false;
    }
    return true;
}

static bool removeResolvedPath(const String& commandName, const String& resolvedPath, bool recursive) {
    if (pathIsProtectedRoot(resolvedPath)) {
        outLine(commandName + ": refusing to remove " + resolvedPath, C_RED);
        return false;
    }

    RoutedPath r = routePath(resolvedPath);
    if (!ensureMountedForPath(commandName, r)) {
        return false;
    }

    ledPulseStorageRead(r.isSd);
    File entry = r.fs->open(r.realPath);
    if (!entry) {
        outLine(commandName + ": " + resolvedPath + " not found", C_RED);
        return false;
    }

    if (!entry.isDirectory()) {
        entry.close();
        ledPulseStorageWrite(r.isSd);
        if (!r.fs->remove(r.realPath)) {
            outLine(commandName + ": could not remove " + resolvedPath, C_RED);
            return false;
        }
        return true;
    }

    if (!recursive) {
        File child = entry.openNextFile();
        bool hasChild = child;
        if (child) {
            child.close();
        }
        entry.close();
        if (hasChild) {
            outLine(commandName + ": " + resolvedPath + " is a directory (use -r)", C_RED);
            return false;
        }
        ledPulseStorageWrite(r.isSd);
        if (!r.fs->rmdir(r.realPath)) {
            outLine(commandName + ": could not remove directory " + resolvedPath, C_RED);
            return false;
        }
        return true;
    }

    File child = entry.openNextFile();
    while (child) {
        String childName = child.name();
        child.close();
        if (!removeResolvedPath(commandName, childPathOf(resolvedPath, r.isSd, childName), true)) {
            entry.close();
            return false;
        }
        child = entry.openNextFile();
    }
    entry.close();

    ledPulseStorageWrite(r.isSd);
    if (!r.fs->rmdir(r.realPath)) {
        outLine(commandName + ": could not remove directory " + resolvedPath, C_RED);
        return false;
    }
    return true;
}

static bool copyResolvedFile(const String& sourceResolved, const String& destResolved, bool overwrite) {
    RoutedPath source = routePath(sourceResolved);
    RoutedPath dest = routePath(destResolved);
    if (!ensureMountedForPath("cp", source) || !ensureMountedForPath("cp", dest)) {
        return false;
    }

    ledPulseStorageRead(source.isSd);
    File in = source.fs->open(source.realPath, "r");
    if (!in) {
        outLine("cp: " + sourceResolved + " not found", C_RED);
        return false;
    }
    if (in.isDirectory()) {
        in.close();
        outLine("cp: " + sourceResolved + " is a directory", C_RED);
        return false;
    }

    String finalDestResolved = destResolved;
    ledPulseStorageRead(dest.isSd);
    File destProbe = dest.fs->open(dest.realPath);
    if (destProbe && destProbe.isDirectory()) {
        destProbe.close();
        finalDestResolved = destResolved;
        if (!finalDestResolved.endsWith("/")) {
            finalDestResolved += "/";
        }
        finalDestResolved += baseNameOf(sourceResolved);
        dest = routePath(finalDestResolved);
    } else if (destProbe) {
        destProbe.close();
    }

    if (pathIsProtectedRoot(finalDestResolved)) {
        in.close();
        outLine("cp: invalid destination " + finalDestResolved, C_RED);
        return false;
    }

    ledPulseStorageRead(dest.isSd);
    File existing = dest.fs->open(dest.realPath);
    if (existing) {
        bool isDir = existing.isDirectory();
        existing.close();
        if (isDir) {
            in.close();
            outLine("cp: " + finalDestResolved + " is a directory", C_RED);
            return false;
        }
        if (!overwrite) {
            in.close();
            outLine("cp: " + finalDestResolved + " already exists", C_RED);
            return false;
        }
        dest.fs->remove(dest.realPath);
    }

    String parent = parentPathOf(finalDestResolved);
    if (!directoryExists(parent)) {
        in.close();
        outLine("cp: destination directory not found: " + parent, C_RED);
        return false;
    }

    ledPulseStorageWrite(dest.isSd);
    File out = dest.fs->open(dest.realPath, "w");
    if (!out) {
        in.close();
        outLine("cp: could not create " + finalDestResolved, C_RED);
        return false;
    }

    uint8_t buffer[256];
    while (in.available()) {
        size_t readCount = in.read(buffer, sizeof(buffer));
        ledPulseStorageRead(source.isSd);
        if (readCount == 0) {
            break;
        }
        if (out.write(buffer, readCount) != readCount) {
            out.close();
            in.close();
            dest.fs->remove(dest.realPath);
            outLine("cp: write failed for " + finalDestResolved, C_RED);
            return false;
        }
        ledPulseStorageWrite(dest.isSd);
        delay(1);
    }

    out.close();
    in.close();
    return true;
}

//Non-recursive filesystem primitives exposed to AppRunner. They reuse the shell's
//routing and copy checks without exposing the shell dispatcher itself.
bool dappStorageMkdir(const String& resolvedPath) {
    if (resolvedPath == "/" || resolvedPath == SD_MOUNT) return false;
    RoutedPath routed = routePath(resolvedPath);
    if (!ensureMountedForPath("mkdir", routed) || directoryExists(resolvedPath)) return false;
    if (!directoryExists(parentPathOf(resolvedPath))) return false;
    ledPulseStorageWrite(routed.isSd);
    return routed.fs->mkdir(routed.realPath);
}

bool dappStorageCopy(const String& sourceResolved, const String& destResolved) {
    return copyResolvedFile(sourceResolved, destResolved, false);
}

bool dappStorageMove(const String& sourceResolved, const String& destResolved) {
    if (!copyResolvedFile(sourceResolved, destResolved, false)) return false;
    RoutedPath source = routePath(sourceResolved);
    ledPulseStorageWrite(source.isSd);
    if (source.fs->remove(source.realPath)) return true;
    RoutedPath destination = routePath(destResolved);
    destination.fs->remove(destination.realPath);
    return false;
}

void handleLsCommand(const String parts[], int partCount) {
    String target = (partCount > 1) ? parts[1] : "";
    String resolved = resolvePath(cwd, target);

    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        outLine("SD not mounted (insert card and reboot)", C_RED);
        return;
    }

    outLine(resolved);
    listDirectory(*r.fs, r.realPath, !r.isSd && resolved == "/" && sdCardMounted, r.isSd);
}

void handleCdCommand(const String parts[], int partCount) {
    String target = (partCount > 1) ? parts[1] : "/";
    String resolved = resolvePath(cwd, target);

    if (!directoryExists(resolved)) {
        outLine("cd: " + resolved + " not found", C_RED);
        return;
    }
    cwd = resolved;
}

void handlePwdCommand(const String parts[], int partCount) {
    outLine(cwd);
}

void handleCatCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: cat <file>");
        return;
    }

    String target = parts[1];
    String resolved = resolvePath(cwd, target);
    RoutedPath r = routePath(resolved);

    if (r.isSd && !sdCardMounted) {
        outLine("SD not mounted (insert card and reboot)", C_RED);
        return;
    }

    File file = r.fs->open(r.realPath, "r");
    ledPulseStorageRead(r.isSd);
    if (!file) {
        outLine("cat: " + resolved + " not found", C_RED);
        return;
    }

    if (file.isDirectory()) {
        outLine("cat: " + resolved + " is a directory", C_RED);
        file.close();
        return;
    }

    if (file.size() == 0) {
        outLine("(empty)");
        file.close();
        return;
    }

    while (file.available()) {
        String line = file.readStringUntil('\n');
        ledPulseStorageRead(r.isSd);
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }
        outLine(line);
    }

    file.close();
}

void handleMkdirCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: mkdir <dir>");
        return;
    }

    String resolved = resolvePath(cwd, parts[1]);
    if (pathIsProtectedRoot(resolved)) {
        outLine("mkdir: invalid path " + resolved, C_RED);
        return;
    }

    RoutedPath r = routePath(resolved);
    if (!ensureMountedForPath("mkdir", r)) {
        return;
    }

    ledPulseStorageRead(r.isSd);
    File existing = r.fs->open(r.realPath);
    if (existing) {
        existing.close();
        outLine("mkdir: " + resolved + " already exists", C_RED);
        return;
    }

    String parent = parentPathOf(resolved);
    if (!directoryExists(parent)) {
        outLine("mkdir: parent not found: " + parent, C_RED);
        return;
    }

    if (!r.fs->mkdir(r.realPath)) {
        ledPulseError();
        outLine("mkdir: could not create " + resolved, C_RED);
        return;
    }
    ledPulseStorageWrite(r.isSd);
    outLine("mkdir: created " + resolved, C_GREEN);
}

void handleRmCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: rm [-r] <path>");
        return;
    }

    bool recursive = false;
    int targetIndex = 1;
    if (parts[1] == "-r" || parts[1] == "-R") {
        recursive = true;
        targetIndex = 2;
    }
    if (targetIndex >= partCount) {
        outLine("Usage: rm [-r] <path>");
        return;
    }

    String resolved = resolvePath(cwd, parts[targetIndex]);
    if (removeResolvedPath("rm", resolved, recursive)) {
        outLine("rm: removed " + resolved, C_GREEN);
    }
}

void handleDelCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: del [-r] <path>");
        return;
    }

    bool recursive = false;
    int targetIndex = 1;
    if (parts[1] == "-r" || parts[1] == "-R") {
        recursive = true;
        targetIndex = 2;
    }
    if (targetIndex >= partCount) {
        outLine("Usage: del [-r] <path>");
        return;
    }

    String resolved = resolvePath(cwd, parts[targetIndex]);
    if (removeResolvedPath("del", resolved, recursive)) {
        outLine("del: removed " + resolved, C_GREEN);
    }
}

void handleCpCommand(const String parts[], int partCount) {
    if (partCount < 3) {
        outLine("Usage: cp <source> <dest>");
        return;
    }

    String sourceResolved = resolvePath(cwd, parts[1]);
    String destResolved = resolvePath(cwd, parts[2]);
    if (copyResolvedFile(sourceResolved, destResolved, false)) {
        outLine("cp: " + sourceResolved + " -> " + destResolved, C_GREEN);
    }
}

void handleMvCommand(const String parts[], int partCount) {
    if (partCount < 3) {
        outLine("Usage: mv <source> <dest>");
        return;
    }

    String sourceResolved = resolvePath(cwd, parts[1]);
    if (pathIsProtectedRoot(sourceResolved)) {
        outLine("mv: refusing to move " + sourceResolved, C_RED);
        return;
    }

    RoutedPath source = routePath(sourceResolved);
    if (!ensureMountedForPath("mv", source)) {
        return;
    }

    ledPulseStorageRead(source.isSd);
    File sourceFile = source.fs->open(source.realPath, "r");
    if (!sourceFile) {
        outLine("mv: " + sourceResolved + " not found", C_RED);
        return;
    }
    bool sourceIsDirectory = sourceFile.isDirectory();
    sourceFile.close();

    String destResolved = resolvePath(cwd, parts[2]);
    RoutedPath dest = routePath(destResolved);
    if (!ensureMountedForPath("mv", dest)) {
        return;
    }

    ledPulseStorageRead(dest.isSd);
    File destProbe = dest.fs->open(dest.realPath);
    if (destProbe && destProbe.isDirectory()) {
        destProbe.close();
        if (!destResolved.endsWith("/")) {
            destResolved += "/";
        }
        destResolved += baseNameOf(sourceResolved);
        dest = routePath(destResolved);
    } else if (destProbe) {
        destProbe.close();
    }

    if (pathIsProtectedRoot(destResolved)) {
        outLine("mv: invalid destination " + destResolved, C_RED);
        return;
    }

    String parent = parentPathOf(destResolved);
    if (!directoryExists(parent)) {
        outLine("mv: destination directory not found: " + parent, C_RED);
        return;
    }

    ledPulseStorageRead(dest.isSd);
    File existing = dest.fs->open(dest.realPath);
    if (existing) {
        existing.close();
        outLine("mv: " + destResolved + " already exists", C_RED);
        return;
    }

    if (source.fs == dest.fs) {
        ledPulseStorageWrite(source.isSd);
        if (!source.fs->rename(source.realPath, dest.realPath)) {
            outLine("mv: could not move " + sourceResolved, C_RED);
            return;
        }
        outLine("mv: " + sourceResolved + " -> " + destResolved, C_GREEN);
        return;
    }

    if (sourceIsDirectory) {
        outLine("mv: cross-filesystem directory moves are not supported", C_RED);
        return;
    }

    if (!copyResolvedFile(sourceResolved, destResolved, false)) {
        return;
    }
    if (!removeResolvedPath("mv", sourceResolved, false)) {
        outLine("mv: copied, but could not remove original " + sourceResolved, C_RED);
        return;
    }
    outLine("mv: " + sourceResolved + " -> " + destResolved, C_GREEN);
}
