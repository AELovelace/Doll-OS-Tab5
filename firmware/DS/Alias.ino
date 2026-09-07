//   Alias.ino
//   Persistent shell command aliases, stored as simple `name=expansion` lines in
//   LittleFS. Expansion happens before command dispatch and preserves any extra
//   arguments the user typed after the alias.
#include <LittleFS.h>

static const char* ALIAS_FILE_PATH = "/system/conf/alias.dsys";
static const char* ALIAS_LEGACY_SYSTEM_DSYS_PATH = "/system/conf/aliases.dsys";
static const char* ALIAS_LEGACY_DSYS_PATH = "/aliases.dsys";
static const char* ALIAS_LEGACY_SINGULAR_DSYS_PATH = "/alias.dsys";
static const char* ALIAS_LEGACY_TXT_PATH = "/aliases.txt";
static const char* ALIAS_TMP_PATH = "/system/conf/alias.tmp.dsys";
static const int ALIAS_MAX_ENTRIES = 32;
static const int ALIAS_NAME_MAX = 24;
static const int ALIAS_EXPANSION_MAX = 160;
static const int ALIAS_EXPANSION_MAX_DEPTH = 4;

static bool aliasNameValid(const String& name) {
    if (name.length() == 0 || name.length() > ALIAS_NAME_MAX) {
        return false;
    }
    for (int i = 0; i < (int)name.length(); i++) {
        char ch = name[i];
        if (ch <= ' ' || ch == '=' || ch == '"' || ch == '\'' || ch == '#') {
            return false;
        }
    }
    return true;
}

static bool aliasReservedName(const String& name) {
    return name == "alias" || name == "unalias" || name == "clear";
}

static int aliasLoadEntries(AliasEntry entries[], int maxEntries) {
    ledPulseStorageRead(false);
    File file = LittleFS.open(ALIAS_FILE_PATH, "r");
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
        String name = line.substring(0, eq);
        String expansion = line.substring(eq + 1);
        name.trim();
        expansion.trim();
        if (!aliasNameValid(name) || expansion.length() == 0) {
            continue;
        }
        if (expansion.length() > ALIAS_EXPANSION_MAX) {
            expansion = expansion.substring(0, ALIAS_EXPANSION_MAX);
        }
        entries[count++] = { name, expansion };
    }
    file.close();
    return count;
}

static bool aliasSaveEntries(const AliasEntry entries[], int count) {
    if (!ensureSystemConfDirectory()) {
        return false;
    }
    ledPulseStorageWrite(false);
    LittleFS.remove(ALIAS_TMP_PATH);
    File file = LittleFS.open(ALIAS_TMP_PATH, "w");
    if (!file) {
        return false;
    }

    file.println("# DOLL-OS command aliases");
    file.println("# Format: name=command expansion");
    for (int i = 0; i < count; i++) {
        if (entries[i].name.length() == 0 || entries[i].expansion.length() == 0) {
            continue;
        }
        file.print(entries[i].name);
        file.print("=");
        file.println(entries[i].expansion);
        ledPulseStorageWrite(false);
    }
    file.close();

    ledPulseStorageWrite(false);
    LittleFS.remove(ALIAS_FILE_PATH);
    if (!LittleFS.rename(ALIAS_TMP_PATH, ALIAS_FILE_PATH)) {
        LittleFS.remove(ALIAS_TMP_PATH);
        return false;
    }
    return true;
}

void ensureDefaultAliases() {
    ensureSystemConfDirectory();
    if (LittleFS.exists(ALIAS_FILE_PATH)) {
        return;
    }
    if (LittleFS.exists(ALIAS_LEGACY_SYSTEM_DSYS_PATH) &&
        LittleFS.rename(ALIAS_LEGACY_SYSTEM_DSYS_PATH, ALIAS_FILE_PATH)) {
        ledPulseStorageWrite(false);
        return;
    }
    if (LittleFS.exists(ALIAS_LEGACY_DSYS_PATH) &&
        LittleFS.rename(ALIAS_LEGACY_DSYS_PATH, ALIAS_FILE_PATH)) {
        ledPulseStorageWrite(false);
        return;
    }
    if (LittleFS.exists(ALIAS_LEGACY_SINGULAR_DSYS_PATH) &&
        LittleFS.rename(ALIAS_LEGACY_SINGULAR_DSYS_PATH, ALIAS_FILE_PATH)) {
        ledPulseStorageWrite(false);
        return;
    }
    if (LittleFS.exists(ALIAS_LEGACY_TXT_PATH) &&
        LittleFS.rename(ALIAS_LEGACY_TXT_PATH, ALIAS_FILE_PATH)) {
        ledPulseStorageWrite(false);
        return;
    }

    AliasEntry entries[1] = {
        { "nano", "edit" },
    };
    aliasSaveEntries(entries, 1);
}

static int aliasFindEntry(const AliasEntry entries[], int count, const String& name) {
    for (int i = 0; i < count; i++) {
        if (entries[i].name == name) {
            return i;
        }
    }
    return -1;
}

static String aliasAppendTail(const String& expansion, const String& original) {
    int start = 0;
    while (start < (int)original.length() && original[start] == ' ') {
        start++;
    }
    while (start < (int)original.length() && original[start] != ' ') {
        start++;
    }
    while (start < (int)original.length() && original[start] == ' ') {
        start++;
    }
    if (start >= (int)original.length()) {
        return expansion;
    }
    return expansion + " " + original.substring(start);
}

static String aliasJoinParts(const String parts[], int partCount, int startIndex) {
    String joined = "";
    for (int i = startIndex; i < partCount; i++) {
        if (parts[i].length() == 0) {
            continue;
        }
        if (joined.length() > 0) {
            joined += " ";
        }
        joined += parts[i];
    }
    joined.trim();
    return joined;
}

bool expandCommandAlias(String& command, String& aliasName, String& aliasExpansion) {
    aliasName = "";
    aliasExpansion = "";

    for (int depth = 0; depth < ALIAS_EXPANSION_MAX_DEPTH; depth++) {
        String parts[2];
        int partCount = splitCommand(command, parts, 2);
        if (partCount == 0) {
            return false;
        }

        AliasEntry entries[ALIAS_MAX_ENTRIES];
        int count = aliasLoadEntries(entries, ALIAS_MAX_ENTRIES);
        int index = aliasFindEntry(entries, count, parts[0]);
        if (index < 0) {
            return aliasName.length() > 0;
        }

        if (aliasName.length() == 0) {
            aliasName = entries[index].name;
            aliasExpansion = entries[index].expansion;
        }
        command = aliasAppendTail(entries[index].expansion, command);
    }

    outLine("alias: expansion stopped after " + String(ALIAS_EXPANSION_MAX_DEPTH) + " levels", C_RED);
    return aliasName.length() > 0;
}

void handleAliasCommand(const String parts[], int partCount) {
    AliasEntry entries[ALIAS_MAX_ENTRIES];
    int count = aliasLoadEntries(entries, ALIAS_MAX_ENTRIES);

    if (partCount == 1) {
        outLine("Aliases", C_CYAN);
        outLine("-------");
        if (count == 0) {
            outLine("(none)");
            outLine("Use: alias <name> <command...>");
            return;
        }
        for (int i = 0; i < count; i++) {
            outLine(entries[i].name + "=" + entries[i].expansion);
        }
        outLine("File: " + String(ALIAS_FILE_PATH), C_CYAN);
        return;
    }

    String sub = parts[1];
    sub.toLowerCase();
    if (sub == "help") {
        outLine("Usage: alias");
        outLine("       alias <name> <command...>");
        outLine("       alias rm <name>");
        outLine("       unalias <name>");
        outLine("Examples:");
        outLine("       alias nano edit");
        outLine("       alias ll ls");
        outLine("Aliases are stored in " + String(ALIAS_FILE_PATH));
        return;
    }

    if (sub == "rm" || sub == "del" || sub == "remove") {
        if (partCount < 3) {
            outLine("Usage: alias rm <name>", C_RED);
            return;
        }
        String name = parts[2];
        int index = aliasFindEntry(entries, count, name);
        if (index < 0) {
            outLine("alias: not found: " + name, C_RED);
            return;
        }
        for (int i = index; i < count - 1; i++) {
            entries[i] = entries[i + 1];
        }
        count--;
        if (!aliasSaveEntries(entries, count)) {
            outLine("alias: could not update " + String(ALIAS_FILE_PATH), C_RED);
            return;
        }
        outLine("alias: removed " + name, C_GREEN);
        return;
    }

    String name = parts[1];
    if (!aliasNameValid(name)) {
        outLine("alias: invalid name (no spaces, quotes, #, or =; max " + String(ALIAS_NAME_MAX) + " chars)", C_RED);
        return;
    }
    if (aliasReservedName(name)) {
        outLine("alias: refusing reserved name: " + name, C_RED);
        return;
    }
    if (partCount < 3) {
        int index = aliasFindEntry(entries, count, name);
        if (index >= 0) {
            outLine(entries[index].name + "=" + entries[index].expansion);
        } else {
            outLine("alias: not found: " + name, C_RED);
        }
        return;
    }

    String expansion = aliasJoinParts(parts, partCount, 2);
    if (expansion.length() == 0 || expansion.length() > ALIAS_EXPANSION_MAX) {
        outLine("alias: expansion must be 1.." + String(ALIAS_EXPANSION_MAX) + " chars", C_RED);
        return;
    }

    String expansionParts[1];
    if (splitCommand(expansion, expansionParts, 1) == 0 || expansionParts[0] == name) {
        outLine("alias: refusing self-referential alias", C_RED);
        return;
    }

    int index = aliasFindEntry(entries, count, name);
    if (index < 0) {
        if (count >= ALIAS_MAX_ENTRIES) {
            outLine("alias: table full (" + String(ALIAS_MAX_ENTRIES) + " entries)", C_RED);
            return;
        }
        index = count++;
    }
    entries[index] = { name, expansion };

    if (!aliasSaveEntries(entries, count)) {
        outLine("alias: could not update " + String(ALIAS_FILE_PATH), C_RED);
        return;
    }
    outLine("alias: " + name + "=" + expansion, C_GREEN);
}

void handleUnaliasCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: unalias <name>", C_RED);
        return;
    }

    String aliasParts[3] = { "alias", "rm", parts[1] };
    handleAliasCommand(aliasParts, 3);
}
