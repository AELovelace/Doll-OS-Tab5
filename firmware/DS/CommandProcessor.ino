//   CommandProcessor.ino
//   parses and runs terminal commands for DOLL-OS. Ported near-verbatim from
//   DOLL-OS's CommandProcessor.ino -- tokenizing, history, and dispatch are all
//   display-agnostic, so only the output calls changed (addWrappedHistoryLine -> outLine).

int splitCommand(const String& input, String parts[], int maxParts) {
    String working = input;
    working.trim();

    int count = 0;
    int start = 0;
    int len = working.length();
    while (start < len && count < maxParts) {
        while (start < len && working[start] == ' ') {
            start++;
        }
        if (start >= len) {
            break;
        }
        char c = working[start];
        if (c == '"' || c == '\'') {
            char quote = c;
            start++;
            int end = working.indexOf(quote, start);
            if (end == -1) {
                parts[count++] = working.substring(start);
                break;
            }
            parts[count++] = working.substring(start, end);
            start = end + 1;
            continue;
        }
        int end = working.indexOf(' ', start);
        if (end == -1) {
            parts[count++] = working.substring(start);
            break;
        }
        parts[count++] = working.substring(start, end);
        start = end + 1;
    }
    return count;
}

static int commandHistoryPhysicalIndex(int logicalIndex) {
    return (commandHistoryHead + logicalIndex) % COMMAND_HISTORY_MAX;
}

void addCommandHistory(const String& cmd) {
    if (cmd.length() == 0) {
        return;
    }
    if (commandHistoryCount < COMMAND_HISTORY_MAX) {
        int slot = commandHistoryPhysicalIndex(commandHistoryCount);
        commandHistory[slot] = cmd;
        commandHistoryCount++;
    } else {
        commandHistory[commandHistoryHead] = cmd;
        commandHistoryHead = (commandHistoryHead + 1) % COMMAND_HISTORY_MAX;
    }
    commandHistoryIndex = -1;
}

void recallCommandHistory(int step, String& text) {
    if (commandHistoryCount == 0) {
        return;
    }

    if (commandHistoryIndex == -1) {
        if (step > 0) {
            return;
        }
        commandHistoryDraft = text;
        commandHistoryIndex = commandHistoryCount - 1;
    } else {
        int newIndex = commandHistoryIndex + step;
        if (newIndex < 0) {
            newIndex = 0;
        } else if (newIndex >= commandHistoryCount) {
            commandHistoryIndex = -1;
            text = commandHistoryDraft;
            return;
        }
        commandHistoryIndex = newIndex;
    }

    text = commandHistory[commandHistoryPhysicalIndex(commandHistoryIndex)];
}

struct CommandEntry {
    const char* name;
    void (*handler)(const String parts[], int partCount);
};

void helpCommandHandler(const String parts[], int partCount) {
    outLine("Commands: alias, apps, asuka, battery, calc, cat, cd, clear, cp, crash, dapper,");
    outLine("          del, dice, edit, free, ftp, gb, help, ip, ls, mkdir, motoko, music, mv,");
    outLine("          ping, pwd, radio, reboot, rm, run, settings, slave, ssh, status,");
    outLine("          telnet, unalias, uptime, wifi");
}

void handleRebootCommand(const String parts[], int partCount) {
    outLine("Restarting...");
    drawDisplayFrame();   //ESP.restart() below never returns to loop(), so paint the panel now --
                           //otherwise a keyboard-driven reboot gives no on-device feedback
    telnetClient.flush();
    delay(500);
    ESP.restart();
}

void handleUptimeCommand(const String parts[], int partCount) {
    unsigned long totalSeconds = millis() / 1000;
    unsigned long days = totalSeconds / 86400;
    unsigned long hours = (totalSeconds % 86400) / 3600;
    unsigned long minutes = (totalSeconds % 3600) / 60;
    unsigned long seconds = totalSeconds % 60;

    char buf[64];
    snprintf(buf, sizeof(buf), "Uptime: %lu days, %02lu:%02lu:%02lu", days, hours, minutes, seconds);
    outLine(String(buf));
}

void handleStatusCommand(const String parts[], int partCount) {
    outLine("");
    outLine("Wi-Fi status", C_CYAN);
    outLine("-----------");
    if (wifiIsConnected() == 1) {
        outLine("Router: connected");
        outLine("Router SSID: " + WiFi.SSID());
        outLine("Station IP: " + WiFi.localIP().toString());
        outLine("Signal: " + String(WiFi.RSSI()) + " dBm");
    } else {
        outLine("Router: disconnected");
    }
    outLine("");
}

//sorted alphabetically for readability; lookup is a linear scan since the table is small
static const CommandEntry commandTable[] = {
    { "alias",  handleAliasCommand },
    { "apps",   handleAppsCommand },
    { "asuka",  handleAsukaCommand },
    { "battery", handleBatteryCommand },
    { "calc",   handleCalcCommand },
    { "cat",    handleCatCommand },
    { "cd",     handleCdCommand },
    { "cp",     handleCpCommand },
    { "crash",  handleCrashCommand },
    { "dapper", handleDapperCommand },
    { "del",    handleDelCommand },
    { "dice",   handleDiceCommand },
    { "edit",   handleEditCommand },
    { "free",   handleFreeCommand },
    { "ftp",    handleFtpCommand },
    { "gb",     handleGbCommand },
    { "help",   helpCommandHandler },
    { "ip",     handleIpCommand },
    { "ls",     handleLsCommand },
    { "mkdir",  handleMkdirCommand },
    { "motoko", handleMotokoCommand },
    { "music",  handleMusicCommand },
    { "mv",     handleMvCommand },
    { "ping",   handlePingCommand },
    { "pwd",    handlePwdCommand },
    { "radio",  handleRadioCommand },
    { "reboot", handleRebootCommand },
    { "rm",     handleRmCommand },
    { "run",    handleRunCommand },
    { "settings", handleSettingsCommand },
    { "slave",  handleSlaveCommand },
    { "ssh",    handleSshCommand },
    { "status", handleStatusCommand },
    { "telnet", handleTelnetCommand },
    { "unalias", handleUnaliasCommand },
    { "uptime", handleUptimeCommand },
    { "wifi",   handleWifiCommand },
};
static const int commandTableSize = sizeof(commandTable) / sizeof(commandTable[0]);

//takes the finished command line, runs it, and clears the buffer for the next entry
void commandProcessor(String& command) {
    displayScrollOffset = 0;   //submitting anything snaps the mirrored panel back to the live tail
    if (command.length() == 0) {
        echoCommandLine("");   //bare Enter still advances a line, like any shell -- and on telnet
                               //this is the newline readTelnetClient() no longer echoes on submit
        return;
    }

    String entered = command;
    command = "";
    commandCursorPos = 0;

    String trimmedEntered = entered;
    trimmedEntered.trim();
    echoCommandLine(trimmedEntered);   //echo before dispatch so the command sits above its own
                                        //output, same order a real shell prints them in
    addCommandHistory(trimmedEntered);

    String dispatchCommand = trimmedEntered;
    String aliasName;
    String aliasExpansion;
    expandCommandAlias(dispatchCommand, aliasName, aliasExpansion);

    String parts[8];
    int partCount = splitCommand(dispatchCommand, parts, 8);
    if (partCount == 0) {
        return;
    }
    String verb = parts[0];
    verb.toLowerCase();
    if (verb == "clear") {
        outClearScreen();
        return;
    }

    for (int i = 0; i < commandTableSize; i++) {
        if (verb == commandTable[i].name) {
            commandTable[i].handler(parts, partCount);
            return;
        }
    }
    String unknown = dispatchCommand;
    if (aliasName.length() > 0) {
        unknown += " (from alias " + aliasName + ")";
    }
    outLine("Unknown command: " + unknown, C_RED);
}
