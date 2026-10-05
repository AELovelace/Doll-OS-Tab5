//   IPTools.ino
//   handles the "ip" command: local IP info, a ping-based subnet scan, and an ARP
//   scan via the esp32ARP library. Ported near-verbatim from DOLL-OS's ip.ino.
#include <ESP32Ping.h>
#include <esp32ARP.h>

static esp32ARP arp;
static bool arpInitialized = false;

//takes a raw byte pointer rather than mac_addr_t for the same hoisted-prototype
//reason DOLL-OS's version notes: this function's prototype gets hoisted above
//IPTools.ino's own #include <esp32ARP.h>
String formatMacAddr(const uint8_t* addr) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return String(buf);
}

void ipComputeRange(byte net[4], byte bcast[4]) {
    IPAddress local = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    for (int i = 0; i < 4; i++) {
        net[i] = local[i] & mask[i];
        bcast[i] = net[i] | (~mask[i] & 0xFF);
    }
}

void ipShowInfo() {
    if (WiFi.status() != WL_CONNECTED) {
        outLine("ip: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }
    outLine("IP: " + WiFi.localIP().toString());
    outLine("Gateway: " + WiFi.gatewayIP().toString());
    outLine("Subnet: " + WiFi.subnetMask().toString());
    outLine("MAC: " + WiFi.macAddress());
    outLine("DNS: " + WiFi.dnsIP().toString());
}

void ipScanNetwork() {
    if (WiFi.status() != WL_CONNECTED) {
        outLine("ip: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    byte net[4], bcast[4];
    ipComputeRange(net, bcast);

    outLine("Scanning " + String(net[0]) + "." + String(net[1]) + "." + String(net[2]) + ".0/24 (this may take a while)");
    telnetClient.flush();

    int found = 0;
    String pending = "";
    for (int h = net[3] + 1; h < bcast[3]; h++) {
        IPAddress target(net[0], net[1], net[2], h);
        ledPulseNetwork();
        if (Ping.ping(target, 1)) {
            found++;
            if (pending.length() == 0) {
                pending = target.toString();
            } else {
                outLine(pending + "  " + target.toString(), C_GREEN);
                pending = "";
            }
        }
    }
    if (pending.length() > 0) {
        outLine(pending, C_GREEN);
    }

    outLine(String(found) + " host(s) responded");
}

void ipArpScan() {
    if (WiFi.status() != WL_CONNECTED) {
        outLine("ip: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    if (!arpInitialized) {
        arp.init();
        arpInitialized = true;
    }

    byte net[4], bcast[4];
    ipComputeRange(net, bcast);

    outLine("ARP scanning " + String(net[0]) + "." + String(net[1]) + "." + String(net[2]) + ".0/24");
    telnetClient.flush();

    for (int h = net[3] + 1; h < bcast[3]; h++) {
        IPAddress target(net[0], net[1], net[2], h);
        ledPulseNetwork();
        arp.sendRequest(target);
        delay(5);
    }
    delay(1000);

    int found = 0;
    for (int h = net[3] + 1; h < bcast[3]; h++) {
        IPAddress target(net[0], net[1], net[2], h);
        mac_addr_t mac;
        if (arp.lookupEntry(target, mac) >= 0) {
            outLine(target.toString() + " -> " + formatMacAddr(mac.addr), C_CYAN);
            found++;
        }
    }

    outLine(String(found) + " host(s) found");
}

//Expected forms: ip | ip scan | ip arp
void handleIpCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        ipShowInfo();
        return;
    }
    if (parts[1] == "scan") {
        ipScanNetwork();
        return;
    }
    if (parts[1] == "arp") {
        ipArpScan();
        return;
    }
    outLine("ip subcommands:");
    outLine("ip");
    outLine("ip scan");
    outLine("ip arp");
}
