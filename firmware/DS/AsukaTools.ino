//   AsukaTools.ino
//   Native tool layer for the ASUKA shell command: live web search, page fetch,
//   current weather, current date/time, and the model-facing tool-call dispatcher.
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

const size_t ASUKA_URL_FETCH_MAX_RESPONSE_BYTES = 12000;
const int ASUKA_URL_FETCH_DEFAULT_MAX_CHARS = 4000;
const int ASUKA_URL_FETCH_MAX_TEXT_CHARS = 6000;

bool asukaClockSyncStarted = false;

static bool asukaIsWhitespaceChar(char ch) {
    return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == '\v';
}

static void asukaDecodeHtmlEntities(String& text) {
    text.replace("&nbsp;", " ");
    text.replace("&amp;", "&");
    text.replace("&lt;", "<");
    text.replace("&gt;", ">");
    text.replace("&quot;", "\"");
    text.replace("&#39;", "'");
}

static void asukaCollapseWhitespace(String& text) {
    String collapsed;
    collapsed.reserve(text.length());
    bool lastWasSpace = true;

    for (size_t i = 0; i < text.length(); i++) {
        char ch = text[i];
        if (asukaIsWhitespaceChar(ch)) {
            if (!lastWasSpace) {
                collapsed += ' ';
                lastWasSpace = true;
            }
            continue;
        }
        collapsed += ch;
        lastWasSpace = false;
    }

    collapsed.trim();
    text = collapsed;
}

static bool asukaContentTypeLooksTextual(String contentType) {
    contentType.toLowerCase();
    return contentType.startsWith("text/") ||
           contentType.indexOf("json") >= 0 ||
           contentType.indexOf("xml") >= 0 ||
           contentType.indexOf("javascript") >= 0 ||
           contentType.indexOf("x-www-form-urlencoded") >= 0;
}

static bool asukaReadHttpBodyWithLimit(void* httpPtr, size_t maxBytes, String& responseBody, bool& truncated) {
    HTTPClient& http = *((HTTPClient*)httpPtr);
    truncated = false;
    responseBody = "";
    responseBody.reserve(maxBytes);

    WiFiClient* stream = http.getStreamPtr();
    if (stream == nullptr) {
        return false;
    }

    int remainingBytes = http.getSize();
    unsigned long lastActivityAt = millis();

    while (http.connected() && (remainingBytes > 0 || remainingBytes == -1)) {
        while (stream->available()) {
            int nextByte = stream->read();
            ledPulseNetwork();
            if (nextByte < 0) {
                break;
            }
            responseBody += (char)nextByte;
            if (remainingBytes > 0) {
                remainingBytes--;
            }
            lastActivityAt = millis();
            if (responseBody.length() >= maxBytes) {
                truncated = true;
                return true;
            }
        }

        if (remainingBytes == 0 || !http.connected()) {
            break;
        }
        if (millis() - lastActivityAt > 2000) {
            break;
        }
        asukaServiceUi();
    }

    return true;
}

static String asukaHtmlToPlainText(const String& html, int maxChars) {
    String extracted;
    extracted.reserve(maxChars);

    bool insideTag = false;
    bool insideScript = false;
    bool insideStyle = false;
    bool lastWasSpace = true;
    String currentTag;

    for (size_t i = 0; i < html.length(); i++) {
        char ch = html[i];

        if (insideTag) {
            if (ch == '>') {
                insideTag = false;
                String tag = currentTag;
                tag.trim();
                tag.toLowerCase();
                if (tag.startsWith("script")) {
                    insideScript = true;
                } else if (tag.startsWith("/script")) {
                    insideScript = false;
                    lastWasSpace = true;
                } else if (tag.startsWith("style")) {
                    insideStyle = true;
                } else if (tag.startsWith("/style")) {
                    insideStyle = false;
                    lastWasSpace = true;
                }
                currentTag = "";
                continue;
            }
            if (currentTag.length() < 32) {
                currentTag += ch;
            }
            continue;
        }

        if (ch == '<') {
            insideTag = true;
            currentTag = "";
            continue;
        }
        if (insideScript || insideStyle) {
            continue;
        }
        if (asukaIsWhitespaceChar(ch)) {
            if (!lastWasSpace && extracted.length() < (size_t)maxChars) {
                extracted += ' ';
                lastWasSpace = true;
            }
            continue;
        }

        extracted += ch;
        lastWasSpace = false;
        if (extracted.length() >= (size_t)maxChars) {
            break;
        }
    }

    asukaDecodeHtmlEntities(extracted);
    asukaCollapseWhitespace(extracted);
    return extracted;
}

static bool asukaFetchUrlContent(String requestedUrl, int requestedMaxChars, String& toolResult) {
    requestedUrl.trim();
    if (requestedUrl.isEmpty()) {
        toolResult = "{\"error\":\"URL cannot be empty.\"}";
        return false;
    }
    if (!requestedUrl.startsWith("http://") && !requestedUrl.startsWith("https://")) {
        toolResult = "{\"error\":\"Only http:// and https:// URLs are supported.\"}";
        return false;
    }

    int maxChars = constrain(requestedMaxChars <= 0 ? ASUKA_URL_FETCH_DEFAULT_MAX_CHARS : requestedMaxChars, 500, ASUKA_URL_FETCH_MAX_TEXT_CHARS);
    bool secure = requestedUrl.startsWith("https://");
    const char* headerKeys[] = {"Content-Type", "Location"};

    HTTPClient http;
    http.setTimeout(15000);
    http.collectHeaders(headerKeys, 2);
    WiFiClient plainClient;
    WiFiClientSecure secureClient;

    bool started = false;
    if (secure) {
        secureClient.setInsecure();
        started = http.begin(secureClient, requestedUrl);
    } else {
        started = http.begin(plainClient, requestedUrl);
    }
    if (!started) {
        toolResult = "{\"error\":\"Could not initialize URL fetch request.\"}";
        return false;
    }

    http.addHeader("Accept", "text/plain, text/html, application/json, application/xml;q=0.9, text/xml;q=0.9");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("User-Agent", "ASUKA-DOLL-OS/1.0");

    ledPulseNetwork();
    int httpCode = http.GET();
    if (httpCode <= 0) {
        toolResult = "{\"error\":\"URL fetch request failed.\"}";
        http.end();
        return false;
    }
    if (httpCode != HTTP_CODE_OK) {
        JsonDocument errorDoc;
        errorDoc["error"] = String("URL fetch returned HTTP ") + httpCode;
        String errorBody = http.getString();
        if (!errorBody.isEmpty()) {
            errorDoc["details"] = errorBody.substring(0, 500);
        }
        serializeJson(errorDoc, toolResult);
        http.end();
        return false;
    }

    String contentType = http.header("Content-Type");
    if (!asukaContentTypeLooksTextual(contentType)) {
        JsonDocument errorDoc;
        errorDoc["error"] = "URL fetch returned a non-text content type.";
        errorDoc["content_type"] = contentType;
        serializeJson(errorDoc, toolResult);
        http.end();
        return false;
    }

    bool bodyTruncated = false;
    String body;
    if (!asukaReadHttpBodyWithLimit(&http, ASUKA_URL_FETCH_MAX_RESPONSE_BYTES, body, bodyTruncated)) {
        toolResult = "{\"error\":\"Could not read the URL fetch response body.\"}";
        http.end();
        return false;
    }
    String redirectedUrl = http.header("Location");
    http.end();

    if (body.isEmpty()) {
        toolResult = "{\"error\":\"URL fetch returned an empty response body.\"}";
        return false;
    }

    String loweredContentType = contentType;
    loweredContentType.toLowerCase();
    String extracted = loweredContentType.indexOf("html") >= 0
        ? asukaHtmlToPlainText(body, maxChars)
        : body.substring(0, maxChars);
    if (loweredContentType.indexOf("html") < 0) {
        asukaDecodeHtmlEntities(extracted);
        asukaCollapseWhitespace(extracted);
    }

    JsonDocument output;
    output["url"] = requestedUrl;
    output["content_type"] = contentType;
    output["http_status"] = httpCode;
    output["requested_max_chars"] = maxChars;
    output["response_truncated"] = bodyTruncated;
    output["text_truncated"] = extracted.length() >= (size_t)maxChars;
    output["content"] = extracted;
    if (!redirectedUrl.isEmpty()) {
        output["redirect_location"] = redirectedUrl;
    }
    serializeJson(output, toolResult);
    return true;
}

static String asukaUrlEncode(const String& input) {
    String encoded;
    encoded.reserve(input.length() * 3);
    const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < input.length(); i++) {
        unsigned char ch = input[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded += (char)ch;
        } else {
            encoded += '%';
            encoded += hex[(ch >> 4) & 0x0F];
            encoded += hex[ch & 0x0F];
        }
    }

    return encoded;
}

static bool asukaBraveSearch(const String& query, int resultCount, String& toolResult) {
    if (asukaBraveApiKey.length() == 0) {
        toolResult = "{\"error\":\"Brave Search API key is not configured.\"}";
        return false;
    }

    String url = String(ASUKA_BRAVE_SEARCH_URL) + "?q=" + asukaUrlEncode(query) + "&count=" + String(constrain(resultCount, 1, 10));
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    HTTPClient http;
    if (!http.begin(secureClient, url)) {
        toolResult = "{\"error\":\"Could not initialize Brave Search request.\"}";
        return false;
    }

    http.setTimeout(15000);
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("X-Subscription-Token", asukaBraveApiKey);

    ledPulseNetwork();
    int httpCode = http.GET();
    String body = http.getString();
    ledPulseNetwork();
    http.end();

    if (httpCode != HTTP_CODE_OK) {
        JsonDocument errorDoc;
        errorDoc["error"] = String("Brave Search returned HTTP ") + httpCode;
        if (!body.isEmpty()) {
            errorDoc["details"] = body.substring(0, 500);
        }
        serializeJson(errorDoc, toolResult);
        return false;
    }

    JsonDocument response;
    DeserializationError err = deserializeJson(response, body);
    if (err) {
        toolResult = "{\"error\":\"Brave Search JSON parsing failed.\"}";
        return false;
    }

    JsonDocument output;
    output["query"] = query;
    JsonArray results = output["results"].to<JsonArray>();
    JsonArray webResults = response["web"]["results"].as<JsonArray>();
    int emitted = 0;
    for (JsonObject result : webResults) {
        if (emitted >= resultCount) {
            break;
        }
        JsonObject row = results.add<JsonObject>();
        row["title"] = result["title"] | "";
        row["url"] = result["url"] | "";
        row["description"] = result["description"] | "";
        emitted++;
    }

    serializeJson(output, toolResult);
    return true;
}

static bool asukaOpenWeatherCurrent(String requestedUnits, String& toolResult) {
    if (asukaOpenWeatherApiKey.length() == 0) {
        toolResult = "{\"error\":\"OpenWeather API key is not configured.\"}";
        return false;
    }

    requestedUnits.trim();
    requestedUnits.toLowerCase();
    if (requestedUnits.isEmpty()) {
        requestedUnits = "metric";
    }
    if (requestedUnits != "imperial" && requestedUnits != "metric") {
        toolResult = "{\"error\":\"Weather units must be metric or imperial.\"}";
        return false;
    }

    String url = String(ASUKA_OPENWEATHER_WEATHER_URL) +
        "?lat=" + String(asukaWeatherLat, 6) +
        "&lon=" + String(asukaWeatherLon, 6) +
        "&appid=" + asukaUrlEncode(asukaOpenWeatherApiKey) +
        "&units=" + requestedUnits;

    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    HTTPClient http;
    if (!http.begin(secureClient, url)) {
        toolResult = "{\"error\":\"Could not initialize OpenWeather request.\"}";
        return false;
    }

    http.setTimeout(15000);
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("User-Agent", "ASUKA-DOLL-OS/1.0");

    ledPulseNetwork();
    int httpCode = http.GET();
    String body = http.getString();
    ledPulseNetwork();
    http.end();

    if (httpCode != HTTP_CODE_OK) {
        JsonDocument errorDoc;
        errorDoc["error"] = String("OpenWeather returned HTTP ") + httpCode;
        if (!body.isEmpty()) {
            errorDoc["details"] = body.substring(0, 500);
        }
        serializeJson(errorDoc, toolResult);
        return false;
    }

    JsonDocument response;
    DeserializationError err = deserializeJson(response, body);
    if (err) {
        toolResult = "{\"error\":\"OpenWeather JSON parsing failed.\"}";
        return false;
    }

    const char* temperatureUnit = requestedUnits == "metric" ? "C" : "F";
    const char* speedUnit = requestedUnits == "metric" ? "m/s" : "mph";

    JsonDocument output;
    output["requested_location"] = asukaWeatherLocationLabel;
    output["units"] = requestedUnits;
    output["provider_location"] = response["name"] | "";
    JsonObject location = output["location"].to<JsonObject>();
    location["label"] = asukaWeatherLocationLabel;
    location["lat"] = asukaWeatherLat;
    location["lon"] = asukaWeatherLon;
    JsonObject weather = output["weather"].to<JsonObject>();
    weather["summary"] = response["weather"][0]["main"] | "";
    weather["description"] = response["weather"][0]["description"] | "";
    JsonObject temperature = output["temperature"].to<JsonObject>();
    temperature["current"] = response["main"]["temp"] | 0.0;
    temperature["feels_like"] = response["main"]["feels_like"] | 0.0;
    temperature["unit"] = temperatureUnit;
    output["humidity_percent"] = response["main"]["humidity"] | 0;
    JsonObject wind = output["wind"].to<JsonObject>();
    wind["speed"] = response["wind"]["speed"] | 0.0;
    wind["unit"] = speedUnit;
    wind["degrees"] = response["wind"]["deg"] | 0;
    output["clouds_percent"] = response["clouds"]["all"] | 0;
    output["visibility_meters"] = response["visibility"] | 0;
    output["source"] = "openweathermap";
    serializeJson(output, toolResult);
    return true;
}

static bool asukaDeviceClockReady() {
    return time(nullptr) >= 1700000000;
}

static void asukaStartClockSync() {
    if (asukaClockSyncStarted) {
        return;
    }
    configTzTime(ASUKA_DEVICE_TIME_ZONE, ASUKA_NTP_SERVER_PRIMARY, ASUKA_NTP_SERVER_SECONDARY);
    asukaClockSyncStarted = true;
}

static bool asukaCurrentDateTime(String& toolResult) {
    asukaStartClockSync();

    unsigned long started = millis();
    while (!asukaDeviceClockReady() && millis() - started < 5000) {
        asukaServiceUi();
    }

    if (!asukaDeviceClockReady()) {
        toolResult = "{\"error\":\"Device clock is not synchronized yet.\"}";
        return false;
    }

    time_t now = time(nullptr);
    struct tm localTimeInfo;
    struct tm utcTimeInfo;
    if (localtime_r(&now, &localTimeInfo) == nullptr || gmtime_r(&now, &utcTimeInfo) == nullptr) {
        toolResult = "{\"error\":\"Could not convert the current timestamp.\"}";
        return false;
    }

    char localIso[32], utcIso[32], localDate[16], localTimeOfDay[16], weekday[16], month[16], tzName[16];
    strftime(localIso, sizeof(localIso), "%Y-%m-%dT%H:%M:%S%z", &localTimeInfo);
    strftime(utcIso, sizeof(utcIso), "%Y-%m-%dT%H:%M:%SZ", &utcTimeInfo);
    strftime(localDate, sizeof(localDate), "%Y-%m-%d", &localTimeInfo);
    strftime(localTimeOfDay, sizeof(localTimeOfDay), "%H:%M:%S", &localTimeInfo);
    strftime(weekday, sizeof(weekday), "%A", &localTimeInfo);
    strftime(month, sizeof(month), "%B", &localTimeInfo);
    strftime(tzName, sizeof(tzName), "%Z", &localTimeInfo);

    JsonDocument output;
    output["unix_epoch"] = (long long)now;
    output["local_iso8601"] = localIso;
    output["utc_iso8601"] = utcIso;
    output["local_date"] = localDate;
    output["local_time"] = localTimeOfDay;
    output["weekday"] = weekday;
    output["month"] = month;
    output["year"] = localTimeInfo.tm_year + 1900;
    output["day_of_month"] = localTimeInfo.tm_mday;
    output["timezone"] = tzName;
    output["timezone_config"] = ASUKA_DEVICE_TIME_ZONE;
    output["source"] = "ntp";
    output["uptime_ms"] = millis();
    serializeJson(output, toolResult);
    return true;
}

bool asukaHandleToolCall(const String& jsonInput, String& toolResult) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonInput);
    if (err) {
        toolResult = "{\"error\":\"Invalid tool-call JSON.\"}";
        return false;
    }

    const char* toolName = doc["tool"] | "";
    if (strcmp(toolName, "brave_search") == 0) {
        if (!asukaBraveSearchEnabled) {
            toolResult = "{\"error\":\"brave_search is currently disabled.\"}";
            return false;
        }
        const char* query = doc["arguments"]["query"] | "";
        int count = doc["arguments"]["count"] | 5;
        if (strlen(query) == 0) {
            toolResult = "{\"error\":\"brave_search requires a query.\"}";
            return false;
        }
        return asukaBraveSearch(String(query), count, toolResult);
    }

    if (strcmp(toolName, "fetch_url") == 0) {
        const char* url = doc["arguments"]["url"] | "";
        int maxChars = doc["arguments"]["max_chars"] | ASUKA_URL_FETCH_DEFAULT_MAX_CHARS;
        if (strlen(url) == 0) {
            toolResult = "{\"error\":\"fetch_url requires a url.\"}";
            return false;
        }
        return asukaFetchUrlContent(String(url), maxChars, toolResult);
    }

    if (strcmp(toolName, "openweather_current") == 0) {
        const char* units = doc["arguments"]["units"] | "metric";
        return asukaOpenWeatherCurrent(String(units), toolResult);
    }

    if (strcmp(toolName, "current_datetime") == 0) {
        return asukaCurrentDateTime(toolResult);
    }

    toolResult = "{\"error\":\"Unknown tool.\"}";
    return false;
}
