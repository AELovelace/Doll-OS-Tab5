//   Settings.ino
//   Generic persisted runtime settings, stored as `key=value` lines in
//   /system/conf/settings.dsys -- same file shape as Alias.ino's alias.dsys. Lets
//   any config.h compile-time default become a runtime-overridable value that
//   survives a firmware reflash (the settings file lives in the spiffs partition,
//   untouched by an app0-only firmware update), with one settingsGet() call at
//   its point of use. See config.h for the compiled-in fallback each key reads.
#include <LittleFS.h>

static const char* SETTINGS_FILE_PATH = "/system/conf/settings.dsys";
static const char* SETTINGS_TMP_PATH = "/system/conf/settings.tmp.dsys";
static const int SETTINGS_MAX_ENTRIES = 32;
static const int SETTINGS_KEY_MAX = 32;
static const int SETTINGS_VALUE_MAX = 160;

static bool settingsKeyValid(const String& key) {
    if (key.length() == 0 || key.length() > SETTINGS_KEY_MAX) {
        return false;
    }
    for (int i = 0; i < (int)key.length(); i++) {
        char ch = key[i];
        if (ch <= ' ' || ch == '=' || ch == '"' || ch == '\'' || ch == '#') {
            return false;
        }
    }
    return true;
}

static int settingsLoadEntries(SettingsEntry entries[], int maxEntries) {
    ledPulseStorageRead(false);
    File file = LittleFS.open(SETTINGS_FILE_PATH, "r");
    if (!file) {
        return 0;
    }

    int count = 0;
    while (file.available() && count < maxEntries) {
        String line = file.readStringUntil('\n');
        ledPulseStorageRead(false);
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }
        line.trim();
        if (line.length() == 0 || line[0] == '#') {
            continue;
        }

        int eq = line.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        String key = line.substring(0, eq);
        String value = line.substring(eq + 1);
        key.trim();
        value.trim();
        if (!settingsKeyValid(key)) {
            continue;
        }
        if (value.length() > SETTINGS_VALUE_MAX) {
            value = value.substring(0, SETTINGS_VALUE_MAX);
        }
        entries[count++] = { key, value };
    }
    file.close();
    return count;
}

static bool settingsSaveEntries(const SettingsEntry entries[], int count) {
    if (!ensureSystemConfDirectory()) {
        return false;
    }
    ledPulseStorageWrite(false);
    LittleFS.remove(SETTINGS_TMP_PATH);
    File file = LittleFS.open(SETTINGS_TMP_PATH, "w");
    if (!file) {
        return false;
    }

    file.println("# DOLL-OS runtime settings -- overrides config.h defaults");
    file.println("# Format: key=value");
    for (int i = 0; i < count; i++) {
        if (entries[i].key.length() == 0) {
            continue;
        }
        file.print(entries[i].key);
        file.print("=");
        file.println(entries[i].value);
        ledPulseStorageWrite(false);
    }
    file.close();

    ledPulseStorageWrite(false);
    LittleFS.remove(SETTINGS_FILE_PATH);
    if (!LittleFS.rename(SETTINGS_TMP_PATH, SETTINGS_FILE_PATH)) {
        LittleFS.remove(SETTINGS_TMP_PATH);
        return false;
    }
    return true;
}

static int settingsFindEntry(const SettingsEntry entries[], int count, const String& key) {
    for (int i = 0; i < count; i++) {
        if (entries[i].key == key) {
            return i;
        }
    }
    return -1;
}

static bool settingsKeyLooksSecret(const String& key) {
    String lower = key;
    lower.toLowerCase();
    return lower.indexOf("pass") >= 0 || lower.indexOf("key") >= 0;
}

String settingsGet(const String& key, const String& fallback) {
    SettingsEntry entries[SETTINGS_MAX_ENTRIES];
    int count = settingsLoadEntries(entries, SETTINGS_MAX_ENTRIES);
    int index = settingsFindEntry(entries, count, key);
    if (index < 0) {
        return fallback;
    }
    return entries[index].value;
}

bool settingsSet(const String& key, const String& value) {
    if (!settingsKeyValid(key)) {
        return false;
    }
    String trimmedValue = value;
    trimmedValue.trim();
    if (trimmedValue.length() > SETTINGS_VALUE_MAX) {
        trimmedValue = trimmedValue.substring(0, SETTINGS_VALUE_MAX);
    }

    SettingsEntry entries[SETTINGS_MAX_ENTRIES];
    int count = settingsLoadEntries(entries, SETTINGS_MAX_ENTRIES);
    int index = settingsFindEntry(entries, count, key);
    if (index < 0) {
        if (count >= SETTINGS_MAX_ENTRIES) {
            return false;
        }
        index = count++;
    }
    entries[index] = { key, trimmedValue };
    return settingsSaveEntries(entries, count);
}

bool settingsUnset(const String& key) {
    SettingsEntry entries[SETTINGS_MAX_ENTRIES];
    int count = settingsLoadEntries(entries, SETTINGS_MAX_ENTRIES);
    int index = settingsFindEntry(entries, count, key);
    if (index < 0) {
        return false;
    }
    for (int i = index; i < count - 1; i++) {
        entries[i] = entries[i + 1];
    }
    count--;
    return settingsSaveEntries(entries, count);
}

//handles the "settings" command: bare lists all overrides (secrets masked), "get"/"set"/"unset"
//manage one key at a time. Most keys only take effect on the next boot -- each subsystem reads
//its settings via settingsGet() during its own lazy-init, not on every command tick.
void handleSettingsCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        SettingsEntry entries[SETTINGS_MAX_ENTRIES];
        int count = settingsLoadEntries(entries, SETTINGS_MAX_ENTRIES);
        outLine("Settings", C_CYAN);
        outLine("--------");
        if (count == 0) {
            outLine("(none -- all values are using their config.h defaults)");
            outLine("Use: settings set <key> <value>");
            return;
        }
        for (int i = 0; i < count; i++) {
            String value = settingsKeyLooksSecret(entries[i].key) ? "****" : entries[i].value;
            outLine(entries[i].key + "=" + value);
        }
        outLine("File: " + String(SETTINGS_FILE_PATH), C_CYAN);
        return;
    }

    String sub = parts[1];
    sub.toLowerCase();

    if (sub == "help") {
        outLine("Usage: settings");
        outLine("       settings get <key>");
        outLine("       settings set <key> <value...>");
        outLine("       settings unset <key>");
        outLine("Known keys: ftp.user, ftp.pass, motoko.broker, motoko.port,");
        outLine("            radio.url, radio.volume, radio.directory_url,");
        outLine("            asuka.llm_host, asuka.llm_port,");
        outLine("            asuka.llm_path, asuka.brave_key, asuka.owm_key,");
        outLine("            asuka.owm_location, asuka.owm_lat, asuka.owm_lon");
        outLine("Overrides persist in " + String(SETTINGS_FILE_PATH) + " and survive a firmware reflash.");
        return;
    }

    if (sub == "get") {
        if (partCount < 3) {
            outLine("Usage: settings get <key>", C_RED);
            return;
        }
        SettingsEntry entries[SETTINGS_MAX_ENTRIES];
        int count = settingsLoadEntries(entries, SETTINGS_MAX_ENTRIES);
        int index = settingsFindEntry(entries, count, parts[2]);
        if (index < 0) {
            outLine("settings: " + parts[2] + " is unset (using config.h default)", C_YELLOW);
            return;
        }
        outLine(entries[index].key + "=" + entries[index].value);
        return;
    }

    if (sub == "unset") {
        if (partCount < 3) {
            outLine("Usage: settings unset <key>", C_RED);
            return;
        }
        if (!settingsUnset(parts[2])) {
            outLine("settings: " + parts[2] + " not found", C_RED);
            return;
        }
        outLine("settings: " + parts[2] + " unset (reboot to apply)", C_GREEN);
        return;
    }

    if (sub == "set") {
        if (partCount < 4) {
            outLine("Usage: settings set <key> <value...>", C_RED);
            return;
        }
        String key = parts[2];
        if (!settingsKeyValid(key)) {
            outLine("settings: invalid key (no spaces, quotes, #, or =; max " + String(SETTINGS_KEY_MAX) + " chars)", C_RED);
            return;
        }
        String value = "";
        for (int i = 3; i < partCount; i++) {
            if (value.length() > 0) {
                value += " ";
            }
            value += parts[i];
        }
        if (!settingsSet(key, value)) {
            outLine("settings: could not save " + key, C_RED);
            return;
        }
        String displayValue = settingsKeyLooksSecret(key) ? "****" : value;
        outLine("settings: " + key + "=" + displayValue + " (reboot to apply)", C_GREEN);
        return;
    }

    outLine("settings: unknown subcommand: " + sub, C_RED);
    outLine("Usage: settings | settings get|set|unset <key> [value]");
}
