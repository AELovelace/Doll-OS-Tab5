//   Ping.ino
//   handles the "ping" command. Ported verbatim from DOLL-OS's ping.ino aside
//   from the output call.
#include <ESP32Ping.h>

//Expected forms: ping <address> | ping <address> <count>
void handlePingCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: ping <address> <count>");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        outLine("ping: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    String address = parts[1];

    int count = 4;
    if (partCount > 2) {
        count = parts[2].toInt();
        if (count < 1) {
            count = 4;
        } else if (count > 255) {
            count = 255;
        }
    }

    outLine("Pinging " + address + " x" + String(count));
    telnetClient.flush();

    if (Ping.ping(address.c_str(), (byte)count)) {
        outLine("Reply from " + address + ", avg " + String(Ping.averageTime(), 1) + "ms");
    } else {
        outLine("Ping to " + address + " failed", C_RED);
    }
}
