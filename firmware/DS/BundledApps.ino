//   BundledApps.ino
//   Seeds selected built-in files from firmware into LittleFS so a normal sketch
//   upload can refresh built-in apps/docs without a separate filesystem upload step.
#include <LittleFS.h>
#include <FS.h>
#include <string.h>
#include "BundledApps.h"

struct BundledAsset {
    const char* path;
    const char* contents;
};

static void ensureBundledDirectory(const char* path) {
    ledPulseStorageRead(false);
    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        ledPulseStorageWrite(false);
        LittleFS.mkdir(path);
    } else {
        dir.close();
    }
}

static bool bundledAssetMatches(const char* path, const char* contents) {
    ledPulseStorageRead(false);
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return false;
    }

    size_t expectedLen = strlen(contents);
    if ((size_t)file.size() != expectedLen) {
        file.close();
        return false;
    }

    char chunk[128];
    size_t offset = 0;
    while (offset < expectedLen) {
        size_t want = expectedLen - offset;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }

        size_t got = file.readBytes(chunk, want);
        if (got != want || memcmp(chunk, contents + offset, want) != 0) {
            file.close();
            return false;
        }
        offset += want;
    }

    file.close();
    return true;
}

static bool writeBundledAsset(const char* path, const char* contents) {
    ledPulseStorageWrite(false);
    File file = LittleFS.open(path, "w");
    if (!file) {
        return false;
    }

    size_t len = strlen(contents);
    size_t written = file.write((const uint8_t*)contents, len);
    ledPulseStorageWrite(false);
    file.close();
    return written == len;
}

static const char* bundledContentWithoutPackageHeader(const char* contents) {
    const char* cursor = contents;
    while (strncmp(cursor, "# @", 3) == 0) {
        const char* newline = strchr(cursor, '\n');
        if (!newline) return cursor;
        cursor = newline + 1;
    }
    return cursor;
}

void seedBundledApps() {
    ensureBundledDirectory("/system");
    ensureBundledDirectory("/system/apps");
    ensureBundledDirectory("/docs");

    const BundledAsset bundledApps[] = {
        { "/system/apps/adventure.dapp", BUNDLED_APP_ADVENTURE },
        { "/system/apps/tetris.dapp", BUNDLED_APP_TETRIS },
        { "/system/apps/snake.dapp", BUNDLED_APP_SNAKE },
        { "/system/apps/dappchat.dapp", BUNDLED_APP_DAPPCHAT },
        { "/system/apps/dappstore.dapp", BUNDLED_APP_DAPPSTORE },
        { "/docs/dapp.txt", BUNDLED_DOC_DAPP },
    };

    //Older firmware seeded these three files into /apps. Remove an old copy only
    //when its bytes still match the firmware bundle exactly; a user-edited copy is
    //an intentional override and must survive the migration.
    const BundledAsset legacyApps[] = {
        { "/apps/adventure.dapp", BUNDLED_APP_ADVENTURE },
        { "/apps/tetris.dapp", BUNDLED_APP_TETRIS },
        { "/apps/snake.dapp", BUNDLED_APP_SNAKE },
    };
    for (const BundledAsset& legacy : legacyApps) {
        if (bundledAssetMatches(legacy.path, legacy.contents) ||
            bundledAssetMatches(legacy.path, bundledContentWithoutPackageHeader(legacy.contents))) {
            ledPulseStorageWrite(false);
            LittleFS.remove(legacy.path);
        }
    }

    // /system/apps is firmware-owned. Remove retired bundle paths so an ordinary
    // sketch reflash completes app identity renames without leaving duplicates.
    const char* retiredBundledApps[] = {
        "/system/apps/dapper-store.dapp",
    };
    for (const char* path : retiredBundledApps) {
        ledPulseStorageWrite(false);
        LittleFS.remove(path);
    }

    for (const BundledAsset& app : bundledApps) {
        if (bundledAssetMatches(app.path, app.contents)) {
            continue;
        }

        if (writeBundledAsset(app.path, app.contents)) {
            outLine("bundle: synced " + String(app.path) + " to flash", C_GREEN);
        } else {
            outLine("bundle: failed to sync " + String(app.path) + " to flash", C_RED);
        }
    }
}
