#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <stdarg.h>
#include <vector>
#include "bridge.h"

#define BOOT_PIN        0
#define PROMPT          "OpenINET> "
#define DEFAULT_BAUD    460800
#define SETUP_WINDOW_MS 3000              // time after boot to press boot for the setup shell
#define SHELL_MAGIC     0x4F49534CUL      // use rtc to save config

static char   inputBuffer[128];
static size_t bufIndex = 0;
static Preferences prefs;

static RTC_NOINIT_ATTR uint32_t rtcShellFlag;
static bool g_bridgeMode = false;

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
        prefs.remove("ssid");
        prefs.remove("pass");
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

// netstart [baud] giving a baud saves it the saved baud is also used for the automatic start at boot
static void cmdNetstart(const std::vector<String> &a) {
    uint32_t baud = prefs.getUInt("baud", DEFAULT_BAUD);
    if (a.size() >= 2) {
        baud = (uint32_t)a[1].toInt();
        if (baud < 115200 || baud > 921600) {
            cputs("baud must be 115200..921600\n");
            return;
        }
        prefs.putUInt("baud", baud);
    }
    String ssid = prefs.getString("ssid", "");
    if (ssid.length() == 0) {
        cputs("no saved network, run: netconf set \"<ssid>\" \"<password>\"\n");
        return;
    }
    cprintf("bridge mode on \"%s\" at %u baud, no more text output.\n"
    "Press BOOT to return to the shell. Without a key press the bridge now starts by itself at boot.\n",
            ssid.c_str(), (unsigned)baud);
    Serial.flush();
    g_bridgeMode = true;
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

// reboot and stay in the setup shell
void rebootToShell() {
    rtcShellFlag = SHELL_MAGIC;
    Serial.flush();
    ESP.restart();
}

// short press in bridge mode > reboot into the setup shell
//note: dont be dumb pressing boot key before boot will throw you in download mode
// has to be pressed after boot inside the timeout window
void checkBootButton() {
    static uint32_t pressedSince = 0;
    if (digitalRead(BOOT_PIN) == LOW) {
        if (pressedSince == 0) pressedSince = millis();
    } else {
        if (pressedSince && millis() - pressedSince > 50) {
            if (g_bridgeMode) rebootToShell();
            else ESP.restart();
        }
        pressedSince = 0;
    }
}

void setup() {
    pinMode(BOOT_PIN, INPUT_PULLUP);
    Serial.begin(115200);

    prefs.begin("openinet", false);
    WiFi.persistent(false);          // manage credentials yourself hmph!
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);

    String ssid = prefs.getString("ssid", "");

    bool shell = false;
    if (esp_reset_reason() == ESP_RST_SW && rtcShellFlag == SHELL_MAGIC) shell = true;   // asked for by boot/exit
    rtcShellFlag = 0;
    if (ssid.length() == 0) shell = true;                                                // nothing to bridge yet

    // setup key wait a moment for boot otherwise go straight to the bridge
    if (!shell) {
        cputs("\nOpenINET: press BOOT within 3 s for setup\n");
        uint32_t t0 = millis();
        while (millis() - t0 < SETUP_WINDOW_MS) {
            if (digitalRead(BOOT_PIN) == LOW) {
                delay(40);
                if (digitalRead(BOOT_PIN) == LOW) {
                    shell = true;
                    while (digitalRead(BOOT_PIN) == LOW) delay(10);   // wait for release or it would reboot
                    break;
                }
            }
            delay(5);
        }
    }

    if (!shell) {
        g_bridgeMode = true;
        bridgeRun(prefs.getUInt("baud", DEFAULT_BAUD), ssid, prefs.getString("pass", ""));   // does not return
    }

    cputs("\nOpenINET project\n\n");
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
