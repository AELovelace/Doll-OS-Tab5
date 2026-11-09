//   Asuka.ino
//   Native DOLL-OS command for local ASUKA text chat. This keeps ASUKA as firmware, not
//   a .dapp: scripts can be interactive, but the LLM transport stays in C++ where
//   sockets, streaming, and heap limits are easier to control.
#include <LittleFS.h>

String asukaLlmHost = "";
uint16_t asukaLlmPort = ASUKA_DEFAULT_LLM_PORT;
String asukaLlmPath = "";
String asukaSystemPrompt = "";
String asukaClassifierPrompt = "";
String asukaWeatherLocationLabel = "";
double asukaWeatherLat = ASUKA_OPENWEATHER_LAT;
double asukaWeatherLon = ASUKA_OPENWEATHER_LON;
String asukaBraveApiKey = "";
String asukaOpenWeatherApiKey = "";
bool asukaBraveSearchEnabled = true;
static bool asukaDefaultsInitialized = false;

const int ASUKA_HISTORY_MAX = 6;
String asukaHistory[ASUKA_HISTORY_MAX];
String asukaInputBuffer = "";
DisplayStreamState asukaDisplayStream;

const char* ASUKA_SYSTEM_PROMPT_FILE = "/system/conf/asuka-system.dsys";
const char* ASUKA_CLASSIFIER_PROMPT_FILE = "/system/conf/asuka-classifier.dsys";
const char* ASUKA_LEGACY_SYSTEM_PROMPT_FILE = "/asuka-system.txt";
const char* ASUKA_LEGACY_CLASSIFIER_PROMPT_FILE = "/asuka-classifier.txt";

//Global String constructors run before setup() enables the PSRAM-backed general
//heap. Delay their payloads until ASUKA is actually launched so its long classifier
//prompt cannot permanently occupy scarce internal heap from boot. This is also the
//lazy-init point for settingsGet() overrides -- LittleFS isn't mounted yet when this
//file's global initializers above run, so a "settings set asuka.*" value has to be
//read here instead, same as radioEnsureDefaults() in Radio.ino.
static void asukaEnsureDefaults() {
    if (asukaDefaultsInitialized) {
        return;
    }
    asukaLlmHost = settingsGet("asuka.llm_host", ASUKA_DEFAULT_LLM_HOST);
    asukaLlmPort = (uint16_t)settingsGet("asuka.llm_port", String(ASUKA_DEFAULT_LLM_PORT)).toInt();
    asukaLlmPath = settingsGet("asuka.llm_path", ASUKA_DEFAULT_LLM_PATH);
    asukaSystemPrompt = ASUKA_SYSTEM_PROMPT;
    asukaClassifierPrompt = ASUKA_CLASSIFIER_PROMPT;
    asukaWeatherLocationLabel = settingsGet("asuka.owm_location", ASUKA_OPENWEATHER_LOCATION_LABEL);
    asukaWeatherLat = settingsGet("asuka.owm_lat", String(ASUKA_OPENWEATHER_LAT, 6)).toDouble();
    asukaWeatherLon = settingsGet("asuka.owm_lon", String(ASUKA_OPENWEATHER_LON, 6)).toDouble();
    asukaBraveApiKey = settingsGet("asuka.brave_key", ASUKA_BRAVE_API_KEY);
    asukaOpenWeatherApiKey = settingsGet("asuka.owm_key", ASUKA_OPENWEATHER_API_KEY);
    asukaDefaultsInitialized = true;
}

static String asukaPrompt() {
    return "asuka> ";
}

static void asukaServiceUi() {
    ftpService();
    radioService();
    maintainInternetConnection();
    ledService();
    drawDisplayFrame();
    delay(1);
}

static bool asukaWritePromptFile(const char* path, const String& promptText) {
    if (!ensureSystemConfDirectory()) {
        return false;
    }
    ledPulseStorageWrite(false);
    File file = LittleFS.open(path, "w");
    if (!file) {
        return false;
    }

    file.print(promptText);
    file.close();
    return true;
}

static bool asukaMigratePromptFile(const char* legacyPath, const char* currentPath) {
    if (LittleFS.exists(currentPath)) {
        return true;
    }
    if (!ensureSystemConfDirectory()) {
        return false;
    }
    if (!LittleFS.exists(legacyPath)) {
        return false;
    }
    ledPulseStorageWrite(false);
    return LittleFS.rename(legacyPath, currentPath);
}

static bool asukaLoadOrSeedPromptFile(const char* path, const char* legacyPath, const char* defaultPrompt,
                                      String& promptText, bool& createdFile) {
    createdFile = false;
    asukaMigratePromptFile(legacyPath, path);

    ledPulseStorageRead(false);
    File file = LittleFS.open(path, "r");
    if (file && !file.isDirectory()) {
        String savedPrompt = file.readString();
        file.close();
        savedPrompt.trim();

        if (savedPrompt.length() > 0) {
            promptText = savedPrompt;
            return true;
        }
    } else if (file) {
        file.close();
    }

    promptText = defaultPrompt;
    createdFile = asukaWritePromptFile(path, promptText);
    return false;
}

static bool asukaSaveSystemPrompt() {
    return asukaWritePromptFile(ASUKA_SYSTEM_PROMPT_FILE, asukaSystemPrompt);
}

static bool asukaSaveClassifierPrompt() {
    return asukaWritePromptFile(ASUKA_CLASSIFIER_PROMPT_FILE, asukaClassifierPrompt);
}

static void asukaReportPromptSaved(const String& promptName, bool saved) {
    if (saved) {
        outLine("asuka: " + promptName + " prompt saved", C_GREEN);
    } else {
        outLine("asuka: " + promptName + " prompt save failed", C_RED);
    }
}

static String asukaPromptLoadStatus(bool loadedFromFile, bool createdFile) {
    if (loadedFromFile) {
        return "loaded from flash";
    }
    return createdFile ? "default seeded to flash" : "default, seed failed";
}

static bool asukaWeatherConfigured() {
    return asukaOpenWeatherApiKey.length() > 0;
}

static String asukaJsonEscape(const String& value) {
    String out;
    out.reserve(value.length() + 16);
    for (int i = 0; i < value.length(); i++) {
        char ch = value[i];
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if ((uint8_t)ch < 0x20) {
            out += ' ';
        } else {
            out += ch;
        }
    }
    return out;
}

static bool asukaIsJsonWhitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static bool asukaJsonFieldString(const String& json, const String& fieldName, String& out) {
    String key = "\"" + fieldName + "\"";
    int keyIndex = json.indexOf(key);
    if (keyIndex < 0) {
        return false;
    }

    int valueIndex = json.indexOf(':', keyIndex + key.length());
    if (valueIndex < 0) {
        return false;
    }
    valueIndex++;
    while (valueIndex < json.length() && asukaIsJsonWhitespace(json[valueIndex])) {
        valueIndex++;
    }
    if (valueIndex >= json.length() || json[valueIndex] != '"') {
        return false;
    }
    valueIndex++;

    out = "";
    bool escaped = false;
    for (int i = valueIndex; i < json.length(); i++) {
        char ch = json[i];
        if (escaped) {
            if (ch == 'n') {
                out += '\n';
            } else if (ch == 'r') {
                out += '\r';
            } else if (ch == 't') {
                out += '\t';
            } else {
                out += ch;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return true;
        }
        out += ch;
    }

    out = "";
    return false;
}

static void asukaAddHistory(const String& sender, const String& message) {
    for (int i = 0; i < ASUKA_HISTORY_MAX - 1; i++) {
        asukaHistory[i] = asukaHistory[i + 1];
    }
    asukaHistory[ASUKA_HISTORY_MAX - 1] = sender + ": " + message;
}

static void asukaClearHistory() {
    for (int i = 0; i < ASUKA_HISTORY_MAX; i++) {
        asukaHistory[i] = "";
    }
}

static String asukaBuildTranscript() {
    String transcript;
    transcript.reserve(768);
    for (int i = 0; i < ASUKA_HISTORY_MAX; i++) {
        if (asukaHistory[i].length() == 0) {
            continue;
        }
        transcript += "> ";
        transcript += asukaHistory[i];
        transcript += "\n";
    }
    return transcript;
}

static String asukaBuildRequestBody(const String& prompt) {
    String body;
    body.reserve(prompt.length() + 512);
    body += "{\"model\":\"model\",\"stream\":true,\"temperature\":0.7,\"max_tokens\":2048,\"messages\":[";
    if (asukaSystemPrompt.length() > 0) {
        body += "{\"role\":\"system\",\"content\":\"";
        body += asukaJsonEscape(asukaSystemPrompt);
        body += "\"},";
    }
    body += "{\"role\":\"user\",\"content\":\"";
    body += asukaJsonEscape(prompt);
    body += "\"}]}";
    return body;
}

static void asukaWriteStreamChunk(const String& chunk) {
    for (int i = 0; i < chunk.length(); i++) {
        char ch = chunk[i];
        if (ch == '\r') {
            displayStreamCarriageReturn(asukaDisplayStream);
            continue;
        }
        if (ch == '\n') {
            if (telnetClient && telnetClient.connected()) {
                telnetClient.print("\r\n");
            }
            displayStreamNewline(asukaDisplayStream);
            continue;
        }

        char shown = ((uint8_t)ch >= 0x80) ? '?' : ch;
        if (telnetClient && telnetClient.connected()) {
            telnetClient.write((uint8_t)shown);
        }
        displayStreamPutChar(asukaDisplayStream, shown, TFT_WHITE);
    }
    drawDisplayFrame();
}

static bool asukaStreamCompletion(const String& prompt, String& responseOut, bool streamToUi = true) {
    responseOut = "";

    WiFiClient client;
    ledPulseNetwork();
    if (!client.connect(asukaLlmHost.c_str(), asukaLlmPort)) {
        ledPulseError();
        outLine("asuka: could not connect to " + asukaLlmHost + ":" + String(asukaLlmPort), C_RED);
        return false;
    }
    client.setTimeout(30000);

    String body = asukaBuildRequestBody(prompt);
    ledPulseNetwork();
    client.print(String("POST ") + asukaLlmPath + " HTTP/1.1\r\n" +
                 "Host: " + asukaLlmHost + ":" + String(asukaLlmPort) + "\r\n" +
                 "Content-Type: application/json\r\n" +
                 "Accept: text/event-stream\r\n" +
                 "Connection: close\r\n" +
                 "Content-Length: " + String(body.length()) + "\r\n\r\n" +
                 body);

    unsigned long started = millis();
    while (client.connected() && !client.available()) {
        if (millis() - started > 30000) {
            client.stop();
            if (streamToUi) {
                outLine("asuka: LLM did not respond before timeout", C_RED);
            }
            return false;
        }
        asukaServiceUi();
    }

    ledPulseNetwork();
    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    if (statusLine.indexOf(" 200 ") < 0) {
        if (streamToUi) {
            outLine("asuka: LLM HTTP error: " + statusLine, C_RED);
        }
        client.stop();
        return false;
    }

    while (client.connected() || client.available()) {
        ledPulseNetwork();
        String headerLine = client.readStringUntil('\n');
        if (headerLine == "\r" || headerLine.length() == 0) {
            break;
        }
    }

    if (streamToUi) {
        displayStreamReset(asukaDisplayStream);
        outLine("ASUKA:", C_PINK);
    }

    bool done = false;
    while (!done && (client.connected() || client.available())) {
        if (!client.available()) {
            asukaServiceUi();
            continue;
        }

        ledPulseNetwork();
        String line = client.readStringUntil('\n');
        line.trim();
        if (!line.startsWith("data:")) {
            continue;
        }

        String payload = line.substring(5);
        payload.trim();
        if (payload == "[DONE]") {
            done = true;
            break;
        }

        String content;
        if (asukaJsonFieldString(payload, "content", content) && content.length() > 0) {
            responseOut += content;
            if (streamToUi) {
                asukaWriteStreamChunk(content);
            }
        }
    }

    if (streamToUi && telnetClient && telnetClient.connected()) {
        telnetClient.print("\r\n");
    }
    if (streamToUi) {
        displayStreamNewline(asukaDisplayStream);
    }
    client.stop();
    return responseOut.length() > 0;
}

static String asukaBuildToolDecisionPrompt(const String& message) {
    String prompt = asukaClassifierPrompt;
    prompt.trim();
    if (!prompt.endsWith(" ")) {
        prompt += ' ';
    }
    prompt += "Runtime tool availability: ";
    prompt += asukaBraveSearchEnabled ? "brave_search enabled; " : "brave_search disabled; ";
    prompt += asukaWeatherConfigured() ? "openweather_current enabled; " : "openweather_current unavailable because the OpenWeather API key is missing; ";
    prompt += "fetch_url enabled; current_datetime enabled. User request: ";
    return prompt + message;
}

static String asukaBuildToolFollowupPrompt(const String& originalMessage, const String& toolResult) {
    return String("Answer the user's request using the tool result below. Do not call any more tools. Be concise and mention useful URLs when relevant. User request: ") +
        originalMessage + "\nTool result JSON: " + toolResult;
}

static bool asukaLooksLikeToolCall(String response) {
    response.trim();
    return response.startsWith("{") && response.indexOf("\"tool\"") >= 0;
}

static String asukaExtractFirstHttpUrl(const String& message) {
    int start = message.indexOf("https://");
    if (start < 0) {
        start = message.indexOf("http://");
    }
    if (start < 0) {
        return "";
    }

    int end = start;
    while (end < message.length()) {
        char ch = message[end];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '"' || ch == '\'' || ch == '<' || ch == '>') {
            break;
        }
        end++;
    }

    String url = message.substring(start, end);
    while (url.endsWith(",") || url.endsWith(".") || url.endsWith(")") || url.endsWith("]")) {
        url.remove(url.length() - 1);
    }
    return url;
}

static bool asukaMessageNeedsCurrentWeather(String message) {
    message.toLowerCase();
    return message.indexOf("weather") >= 0 ||
           message.indexOf("temperature") >= 0 ||
           message.indexOf("forecast") >= 0 ||
           message.indexOf("rain") >= 0 ||
           message.indexOf("snow") >= 0 ||
           message.indexOf("wind") >= 0 ||
           message.indexOf("humidity") >= 0;
}

static String asukaPreferredWeatherUnits(String message) {
    message.toLowerCase();
    if (message.indexOf("imperial") >= 0 || message.indexOf("fahrenheit") >= 0 || message.indexOf(" mph") >= 0) {
        return "imperial";
    }
    return "metric";
}

static bool asukaParseWeatherLocationCommand(const String& line, double& lat, double& lon, String& label) {
    String rest = line.substring(9);
    rest.trim();
    int firstSpace = rest.indexOf(' ');
    if (firstSpace < 0) {
        return false;
    }
    int secondStart = firstSpace + 1;
    while (secondStart < rest.length() && rest[secondStart] == ' ') {
        secondStart++;
    }
    int secondSpace = rest.indexOf(' ', secondStart);
    if (secondSpace < 0) {
        return false;
    }

    lat = rest.substring(0, firstSpace).toDouble();
    lon = rest.substring(secondStart, secondSpace).toDouble();
    label = rest.substring(secondSpace + 1);
    label.trim();
    return label.length() > 0;
}

static bool asukaAskWithTools(const String& originalMessage, const String& prompt, String& responseOut) {
    responseOut = "";

    String firstUrl = asukaExtractFirstHttpUrl(originalMessage);
    if (!firstUrl.isEmpty()) {
        outLine("asuka: fetching page...", C_YELLOW);
        String toolRequest = String("{\"tool\":\"fetch_url\",\"arguments\":{\"url\":\"") + asukaJsonEscape(firstUrl) + "\",\"max_chars\":4000}}";
        String toolResult;
        if (!asukaHandleToolCall(toolRequest, toolResult)) {
            outLine("asuka: " + toolResult, C_RED);
            return false;
        }
        return asukaStreamCompletion(asukaBuildToolFollowupPrompt(prompt, toolResult), responseOut, true);
    }

    if (asukaWeatherConfigured() && asukaMessageNeedsCurrentWeather(originalMessage)) {
        outLine("asuka: checking weather...", C_YELLOW);
        String units = asukaPreferredWeatherUnits(originalMessage);
        String toolRequest = String("{\"tool\":\"openweather_current\",\"arguments\":{\"units\":\"") + units + "\"}}";
        String toolResult;
        if (!asukaHandleToolCall(toolRequest, toolResult)) {
            outLine("asuka: " + toolResult, C_RED);
            return false;
        }
        return asukaStreamCompletion(asukaBuildToolFollowupPrompt(prompt, toolResult), responseOut, true);
    }

    String firstResponse;
    if (!asukaStreamCompletion(asukaBuildToolDecisionPrompt(prompt), firstResponse, false)) {
        outLine("asuka: LLM request failed or timed out.", C_RED);
        return false;
    }

    if (!asukaLooksLikeToolCall(firstResponse)) {
        displayStreamReset(asukaDisplayStream);
        outLine("ASUKA:", C_PINK);
        asukaWriteStreamChunk(firstResponse);
        if (telnetClient && telnetClient.connected()) {
            telnetClient.print("\r\n");
        }
        displayStreamNewline(asukaDisplayStream);
        responseOut = firstResponse;
        return true;
    }

    outLine("asuka: running live tool...", C_YELLOW);
    String toolResult;
    if (!asukaHandleToolCall(firstResponse, toolResult)) {
        outLine("asuka: " + toolResult, C_RED);
        return false;
    }

    return asukaStreamCompletion(asukaBuildToolFollowupPrompt(prompt, toolResult), responseOut, true);
}

static void asukaPrintHelp() {
    outLine("ASUKA commands:", C_CYAN);
    outLine("/help");
    outLine("/status");
    outLine("/host <host-or-ip>");
    outLine("/port <1-65535>");
    outLine("/search on|off");
    outLine("/weather");
    outLine("/weather <lat> <lon> <label>");
    outLine("/system");
    outLine("/system <prompt>");
    outLine("/system reset");
    outLine("/classifier");
    outLine("/classifier <prompt>");
    outLine("/classifier reset");
    outLine("/clear");
    outLine("/quit");
}

static void asukaHandleLine(const String& line, bool& shouldExit) {
    if (line == "/quit" || line == "exit") {
        shouldExit = true;
        return;
    }
    if (line == "/help") {
        asukaPrintHelp();
        return;
    }
    if (line == "/status") {
        outLine("LLM: " + asukaLlmHost + ":" + String(asukaLlmPort) + asukaLlmPath, C_CYAN);
        outLine("WiFi: " + String(wifiIsConnected() == 1 ? "connected" : "disconnected"));
        outLine("Search: " + String(asukaBraveSearchEnabled ? "enabled" : "disabled"));
        outLine("Weather: " + String(asukaWeatherConfigured() ? "enabled" : "missing API key"));
        outLine("Weather location: " + asukaWeatherLocationLabel + " (" + String(asukaWeatherLat, 4) + ", " + String(asukaWeatherLon, 4) + ")");
        outLine("System prompt file: " + String(ASUKA_SYSTEM_PROMPT_FILE));
        outLine("Classifier prompt file: " + String(ASUKA_CLASSIFIER_PROMPT_FILE));
        outLine("System prompt: " + asukaSystemPrompt);
        return;
    }
    if (line == "/clear") {
        asukaClearHistory();
        outLine("asuka: history cleared", C_GREEN);
        return;
    }
    if (line.startsWith("/host ")) {
        asukaLlmHost = line.substring(6);
        asukaLlmHost.trim();
        outLine("asuka: host set to " + asukaLlmHost, C_GREEN);
        return;
    }
    if (line.startsWith("/port ")) {
        int port = line.substring(6).toInt();
        if (port <= 0 || port > 65535) {
            outLine("asuka: invalid port", C_RED);
            return;
        }
        asukaLlmPort = (uint16_t)port;
        outLine("asuka: port set to " + String(asukaLlmPort), C_GREEN);
        return;
    }
    if (line == "/search on" || line == "/search enable") {
        asukaBraveSearchEnabled = true;
        outLine("asuka: Brave search enabled", C_GREEN);
        return;
    }
    if (line == "/search off" || line == "/search disable") {
        asukaBraveSearchEnabled = false;
        outLine("asuka: Brave search disabled", C_GREEN);
        return;
    }
    if (line == "/weather") {
        outLine("Weather: " + String(asukaWeatherConfigured() ? "enabled" : "missing API key"), C_CYAN);
        outLine("Location: " + asukaWeatherLocationLabel);
        outLine("Lat/Lon: " + String(asukaWeatherLat, 6) + ", " + String(asukaWeatherLon, 6));
        return;
    }
    if (line.startsWith("/weather ")) {
        double lat = 0.0;
        double lon = 0.0;
        String label;
        if (!asukaParseWeatherLocationCommand(line, lat, lon, label)) {
            outLine("Usage: /weather <lat> <lon> <label>", C_RED);
            return;
        }
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            outLine("asuka: invalid weather coordinates", C_RED);
            return;
        }
        asukaWeatherLat = lat;
        asukaWeatherLon = lon;
        asukaWeatherLocationLabel = label;
        outLine("asuka: weather location set to " + asukaWeatherLocationLabel, C_GREEN);
        return;
    }
    if (line == "/system") {
        outLine("System prompt file: " + String(ASUKA_SYSTEM_PROMPT_FILE), C_CYAN);
        outLine(asukaSystemPrompt);
        return;
    }
    if (line == "/system reset") {
        asukaSystemPrompt = ASUKA_SYSTEM_PROMPT;
        outLine("asuka: system prompt reset", C_GREEN);
        asukaReportPromptSaved("system", asukaSaveSystemPrompt());
        return;
    }
    if (line.startsWith("/system ")) {
        asukaSystemPrompt = line.substring(8);
        asukaSystemPrompt.trim();
        outLine("asuka: system prompt updated", C_GREEN);
        asukaReportPromptSaved("system", asukaSaveSystemPrompt());
        return;
    }
    if (line == "/classifier") {
        outLine("Classifier prompt file: " + String(ASUKA_CLASSIFIER_PROMPT_FILE), C_CYAN);
        outLine(asukaClassifierPrompt);
        return;
    }
    if (line == "/classifier reset") {
        asukaClassifierPrompt = ASUKA_CLASSIFIER_PROMPT;
        outLine("asuka: classifier prompt reset", C_GREEN);
        asukaReportPromptSaved("classifier", asukaSaveClassifierPrompt());
        return;
    }
    if (line.startsWith("/classifier ")) {
        asukaClassifierPrompt = line.substring(12);
        asukaClassifierPrompt.trim();
        outLine("asuka: classifier prompt updated", C_GREEN);
        asukaReportPromptSaved("classifier", asukaSaveClassifierPrompt());
        return;
    }

    asukaAddHistory("User", line);
    String response;
    if (asukaAskWithTools(line, asukaBuildTranscript(), response)) {
        asukaAddHistory("ASUKA", response);
    }
}

static void runAsukaBlocking() {
    asukaInputBuffer = "";
    commandCursorPos = 0;
    bool shouldExit = false;

    bool systemPromptCreated = false;
    bool classifierPromptCreated = false;
    bool loadedSystemPrompt = asukaLoadOrSeedPromptFile(ASUKA_SYSTEM_PROMPT_FILE, ASUKA_LEGACY_SYSTEM_PROMPT_FILE,
                                                        ASUKA_SYSTEM_PROMPT, asukaSystemPrompt, systemPromptCreated);
    bool loadedClassifierPrompt = asukaLoadOrSeedPromptFile(ASUKA_CLASSIFIER_PROMPT_FILE, ASUKA_LEGACY_CLASSIFIER_PROMPT_FILE,
                                                            ASUKA_CLASSIFIER_PROMPT, asukaClassifierPrompt, classifierPromptCreated);
    outLine("ASUKA local chat", C_PINK);
    outLine("System prompt: " + asukaPromptLoadStatus(loadedSystemPrompt, systemPromptCreated));
    outLine("Classifier prompt: " + asukaPromptLoadStatus(loadedClassifierPrompt, classifierPromptCreated));
    outLine("/quit to exit, /help for commands");
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(asukaPrompt());
    }

    while (!shouldExit) {
        LineInputResult r = readLineEditedInput(asukaInputBuffer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(asukaInputBuffer);
        }

        setActiveInput(asukaPrompt(), asukaInputBuffer, false);
        asukaServiceUi();

        if (r != LINE_SUBMITTED) {
            continue;
        }

        String submitted = asukaInputBuffer;
        asukaInputBuffer = "";
        commandCursorPos = 0;
        submitted.trim();

        if (submitted.length() == 0) {
            if (telnetClient && telnetClient.connected()) {
                telnetClient.print(asukaPrompt());
            }
            continue;
        }

        outLine("> " + submitted, C_CYAN);
        asukaHandleLine(submitted, shouldExit);
        setActiveInput(asukaPrompt(), "", false);

        if (!shouldExit && telnetClient && telnetClient.connected()) {
            telnetClient.print(asukaPrompt());
        }
    }

    displayStreamReset(asukaDisplayStream);
    setActiveInput(shellPrompt(), "", false);
    outLine("asuka: exited");
}

void handleAsukaCommand(const String parts[], int partCount) {
    if (WiFi.status() != WL_CONNECTED) {
        outLine("asuka: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    asukaEnsureDefaults();

    if (partCount > 1) {
        asukaLlmHost = parts[1];
    }
    if (partCount > 2) {
        int port = parts[2].toInt();
        if (port <= 0 || port > 65535) {
            outLine("Usage: asuka [host] [port]", C_RED);
            return;
        }
        asukaLlmPort = (uint16_t)port;
    }

    runAsukaBlocking();
}
