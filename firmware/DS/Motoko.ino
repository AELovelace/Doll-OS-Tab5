//   Motoko.ino
//   MQTT chat client, launched by the "motoko" command. Takes over the telnet
//   session until "/quit" is typed, then hands control back to the shell.
//   Ported from DOLL-OS's motoko.ino: reuses the shared line-editing input reader
//   (readLineEditedInput, TelnetServer.ino) exactly the way DOLL-OS's original
//   reused readKeyboard() -- same buffered Q&A shape, just a different transport
//   underneath the shared input/output primitives.
#include <PubSubClient.h>

WiFiClient motokoWifiClient;
PubSubClient motokoMqttClient(motokoWifiClient);

enum MotokoInputMode { MOTOKO_ASK_CHANNEL, MOTOKO_ASK_MESSAGE };
MotokoInputMode motokoInputMode = MOTOKO_ASK_CHANNEL;

String motokoChannel = "";
String motokoInputBuffer = "";

unsigned long motokoLastReconnectAttempt = 0;
const unsigned long MOTOKO_RECONNECT_INTERVAL_MS = 2000;
bool motokoWasConnected = false;

static String motokoPrompt() {
    return (motokoInputMode == MOTOKO_ASK_CHANNEL) ? "channel> " : "msg> ";
}

//PubSubClient callback: logs incoming messages, "answer" gets a highlight color to
//make the reserved reply topic stand out
void motokoMqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    if (strcmp(topic, "answer") == 0) {
        outLine("[answer] " + msg, C_PINK);
    } else {
        outLine("< " + msg, C_YELLOW);
    }
}

//handles a finished line of input. Anything starting with "/" is treated as a command
//attempt -- never as a channel name or a published message
void motokoHandleEnteredLine() {
    if (motokoInputBuffer.length() == 0) {
        return;
    }

    if (motokoInputBuffer.startsWith("/")) {
        if (motokoInputBuffer == "/battery") {
            outLine("Battery: " + String(readBatteryPercent()) + "% (" + String(readBatteryVoltage(), 2) + "V)", C_CYAN);
        } else if (motokoInputBuffer == "/channel") {
            motokoInputMode = MOTOKO_ASK_CHANNEL;
        } else {
            outLine("Unsupported command: " + motokoInputBuffer, C_RED);
        }
        return;
    }

    if (motokoInputMode == MOTOKO_ASK_CHANNEL) {
        if (motokoChannel.length() > 0) {
            motokoMqttClient.unsubscribe(motokoChannel.c_str());
        }
        motokoChannel = motokoInputBuffer;
        motokoMqttClient.subscribe(motokoChannel.c_str());
        outLine("Channel: " + motokoChannel, C_CYAN);
        motokoInputMode = MOTOKO_ASK_MESSAGE;
    } else {
        bool ok = motokoMqttClient.publish(motokoChannel.c_str(), motokoInputBuffer.c_str());
        outLine((ok ? "> " : "FAILED: ") + motokoInputBuffer, ok ? C_GREEN : C_RED);
    }
}

//the modal loop: owns the telnet session until "/quit" is entered
void runMotokoBlocking() {
    while (true) {
        delay(10);

        motokoMqttClient.loop();
        bool nowConnected = motokoMqttClient.connected();
        if (motokoWasConnected && !nowConnected) {
            outLine("MQTT disconnected", C_RED);
        }
        motokoWasConnected = nowConnected;

        if (!nowConnected && millis() - motokoLastReconnectAttempt > MOTOKO_RECONNECT_INTERVAL_MS) {
            motokoLastReconnectAttempt = millis();
            //setServer() was given a pre-resolved IPAddress and both clients were given short
            //timeouts, so this blocking call is bounded to ~4s instead of PubSubClient's
            //default 3s TCP connect + 15s CONNACK wait
            if (motokoMqttClient.connect(MOTOKO_CLIENT_ID)) {
                outLine("MQTT connected!", C_GREEN);
                motokoWasConnected = true;
                motokoMqttClient.subscribe("answer");
                if (motokoChannel.length() > 0) {
                    motokoMqttClient.subscribe(motokoChannel.c_str());
                }
            }
        }

        //driven by whichever input surface is present -- telnet client or BLE keyboard. This
        //loop blocks loop(), so it polls both itself; a dropped/absent telnet client is no
        //longer a reason to quit (panel mirror + keyboard are a full UI). "/quit" still exits.
        LineInputResult r = readLineEditedInput(motokoInputBuffer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(motokoInputBuffer);
        }
        setActiveInput(motokoPrompt(), motokoInputBuffer, false);
        ledService();
        drawDisplayFrame();   //this loop never returns to DS.ino's loop() until /quit, so the
                               //mirror has to repaint itself here too
        if (r == LINE_SUBMITTED) {
            if (motokoInputBuffer == "/quit") {
                break;
            }
            motokoHandleEnteredLine();
            motokoInputBuffer = "";
            commandCursorPos = 0;
            setActiveInput(motokoPrompt(), motokoInputBuffer, false);
            telnetClient.print(motokoPrompt());
        }
    }

    motokoMqttClient.disconnect();
}

//handles the "motoko" command: motoko [broker-ip] [port]
void handleMotokoCommand(const String parts[], int partCount) {
    if (WiFi.status() != WL_CONNECTED) {
        outLine("motoko: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    //defaults come from config.h unless overridden with "settings set motoko.broker/motoko.port"
    String broker = (partCount > 1) ? parts[1] : settingsGet("motoko.broker", MOTOKO_DEFAULT_BROKER);
    int port = (partCount > 2) ? parts[2].toInt() : settingsGet("motoko.port", String(MOTOKO_DEFAULT_PORT)).toInt();
    if (port <= 0) {
        port = MOTOKO_DEFAULT_PORT;
    }

    //resolve once up front and hand PubSubClient a raw IP instead of a hostname string --
    //otherwise every reconnect attempt in runMotokoBlocking() re-runs DNS
    IPAddress brokerIp;
    if (!WiFi.hostByName(broker.c_str(), brokerIp)) {
        outLine("motoko: could not resolve " + broker, C_RED);
        return;
    }

    motokoInputBuffer = "";
    motokoChannel = "";
    motokoInputMode = MOTOKO_ASK_CHANNEL;
    motokoLastReconnectAttempt = 0;
    motokoWasConnected = false;
    commandCursorPos = 0;

    motokoWifiClient.setTimeout(2);
    motokoMqttClient.setSocketTimeout(2);
    motokoMqttClient.setServer(brokerIp, port);
    motokoMqttClient.setCallback(motokoMqttCallback);
    motokoMqttClient.setBufferSize(1024);

    outLine("MOTOKO " + broker + ":" + String(port), C_PINK);
    outLine("(/quit to exit, /channel to switch channel, /battery for power status)");
    telnetClient.print(motokoPrompt());

    runMotokoBlocking();

    outLine("motoko: exited");
}
