#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <stdarg.h>
#include <vector>
#include "bridge.h"

#define BOOT_PIN     0
// boot button fully resets the thing, wifi config too
#define PROMPT       "OpenINET> "
#define DEFAULT_BAUD 460800

static char   inputBuffer[128];
static size_t bufIndex = 0;
static Preferences prefs;

// fix bs with some terminals tabbing the text
static void cputs(const char *s) {
    while (*s) {
        if (*s == '\n') Serial.write('\r');
        Serial.write(*s++);
    }
}

static void cprintf(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cputs(buf);
}

//helpers
// Split a line into tokens "double quotes" allow spaces
static std::vector<String> tokenize(const String &line) {
    std::vector<String> out;
    String cur;
    bool inQuote = false, have = false;
    for (size_t i = 0; i < line.length(); i++) {
        char c = line[i];
        if (c == '"') { inQuote = !inQuote; have = true; }
        else if (c == ' ' && !inQuote) {
            if (have) { out.push_back(cur); cur = ""; have = false; }
        } else { cur += c; have = true; }
    }
    if (have) out.push_back(cur);
    return out;
}

static bool wifiConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    cputs("connecting");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(300);
        Serial.write('.');
    }
    cputs("\n");
    return WiFi.status() == WL_CONNECTED;
}

static void printStatus() {
    cprintf("MAC:    %s\n", WiFi.macAddress().c_str());
    if (WiFi.status() == WL_CONNECTED) {
        cprintf("SSID:   %s\n", WiFi.SSID().c_str());
        cprintf("IP:     %s\n", WiFi.localIP().toString().c_str());
        cprintf("GW:     %s\n", WiFi.gatewayIP().toString().c_str());
        cprintf("RSSI:   %d dBm\n", WiFi.RSSI());
    } else {
        cputs("WiFi:   not connected\n");
    }
}

// commands
static void cmdNetconf(const std::vector<String> &a) {
    if (a.size() >= 2 && a[1].equalsIgnoreCase("scan")) {
        cputs("scanning...\n");
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
            cprintf("%2d) %-32s %4d dBm  %s\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
        }
        if (n == 0) cputs("no networks found\n");
        WiFi.scanDelete();
    }
    else if (a.size() >= 3 && a[1].equalsIgnoreCase("set")) {
        String ssid = a[2];
        String pass = a.size() >= 4 ? a[3] : "";
        if (wifiConnect(ssid, pass, 15000)) {
            prefs.putString("ssid", ssid);
            prefs.putString("pass", pass);
            cputs("connected, credentials saved\n");
            printStatus();
        } else {
            cputs("failed to connect (credentials NOT saved)\n");
        }
    }
    else if (a.size() >= 2 && a[1].equalsIgnoreCase("forget")) {
        prefs.clear();
        WiFi.disconnect(true);
        cputs("credentials erased\n");
    }
    else if (a.size() >= 2 && a[1].equalsIgnoreCase("status")) {
        printStatus();
    }
    else {
        cputs("usage:\n"
        "  netconf scan\n"
        "  netconf set \"<ssid>\" [\"<password>\"]\n"
        "  netconf forget\n"
        "  netconf status\n");
    }
}

static void cmdNetstart(const std::vector<String> &a) {
    uint32_t baud = DEFAULT_BAUD;
    if (a.size() >= 2) {
        baud = (uint32_t)a[1].toInt();
        if (baud < 115200 || baud > 921600) {
            cputs("baud must be 115200..921600\n");
            return;
        }
    }
    String ssid = prefs.getString("ssid", "");
    if (ssid.length() == 0) {
        cputs("no saved network, run: netconf set \"<ssid>\" \"<password>\"\n");
        return;
    }
    cprintf("bridge mode on \"%s\" at %u baud, no more text output.\n"
    "BOOT button returns to the shell.\n", ssid.c_str(), (unsigned)baud);
    Serial.flush();
    bridgeRun(baud, ssid, prefs.getString("pass", ""));   // does not return
}

static void processCommand(String command) {
    command.trim();
    if (command.length() == 0) return;

    std::vector<String> a = tokenize(command);
    const String &cmd = a[0];

    if (cmd.equalsIgnoreCase("netconf"))       cmdNetconf(a);
    else if (cmd.equalsIgnoreCase("netstart")) cmdNetstart(a);
    else if (cmd.equalsIgnoreCase("netblock")) cputs("not implemented yet\n");
    else if (cmd.equalsIgnoreCase("reset"))    { cputs("restarting...\n"); Serial.flush(); ESP.restart(); }
    else if (cmd.equalsIgnoreCase("help"))     cputs("commands: netconf, netstart [baud], netblock, reset, help\n");
    else                                       cputs("unknown command.\n");
}

void checkBootButton() {
    static uint32_t pressedSince = 0;
    if (digitalRead(BOOT_PIN) == LOW) {
        if (pressedSince == 0) pressedSince = millis();
        else if (millis() - pressedSince > 200) ESP.restart();   // debounced
        prefs.clear();
        WiFi.disconnect(true); // oh noes! it forgot the network!
    } else {
        pressedSince = 0;
    }
}

void setup() {
    pinMode(BOOT_PIN, INPUT_PULLUP);
    Serial.begin(115200);
    cputs("\nOpenINET project\n\n");

    prefs.begin("openinet", false);
    WiFi.persistent(false);          // manage credentials yourself hmph!
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);

    String ssid = prefs.getString("ssid", "");
    if (ssid.length() > 0) {
        cprintf("saved network: %s\n", ssid.c_str());
        WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());  // non blocking
    }
    cputs(PROMPT);
}

void loop() {
    checkBootButton();

    static char lastChar = 0;

    while (Serial.available() > 0) {
        char c = Serial.read();

        if (c == '\r' || c == '\n') {
            if (c == '\n' && lastChar == '\r') { lastChar = c; continue; }
            lastChar = c;

            cputs("\n");
            inputBuffer[bufIndex] = '\0';
            if (bufIndex > 0) processCommand(String(inputBuffer));
            bufIndex = 0;
            cputs(PROMPT);
            continue;
        }
        lastChar = c;

        if (c == '\b' || c == 127) {
            if (bufIndex > 0) { bufIndex--; cputs("\b \b"); }
        }
        else if (c >= 32 && c <= 126 && bufIndex < sizeof(inputBuffer) - 1) {
            inputBuffer[bufIndex++] = c;
            Serial.write(c);
        }
    }
}
