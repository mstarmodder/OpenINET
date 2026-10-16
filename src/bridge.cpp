#include "bridge.h"
#include "proto.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Internal idf api
extern "C" {
    typedef esp_err_t (*oi_rxcb_t)(void *buffer, uint16_t len, void *eb);
    esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t ifx, oi_rxcb_t fn);
    esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len);
    void      esp_wifi_internal_free_rx_buffer(void *buffer);
}

extern void checkBootButton();

struct Frame {
    uint16_t len;
    uint8_t  type;
    uint8_t  data[];
};

// TX queue is limited by bytes
// eat queue slots
constexpr uint32_t OI_TXQ_BYTES  = 24 * 1024;
constexpr uint32_t OI_TXQ_FRAMES = 128;

static QueueHandle_t txq;
static uint8_t       staMac[6];
static volatile bool wifiUp = false;
static volatile uint32_t queuedBytes   = 0;
static volatile uint32_t droppedFrames = 0;
static volatile uint32_t wifiTxFail    = 0;   // diagnostics
static volatile uint32_t uartTxBytes   = 0;   // ^ needed for debug you know
static volatile uint32_t uartRxBytes   = 0;

static bool enqueue(uint8_t type, const uint8_t *data, uint16_t len) {
    if (queuedBytes + len > OI_TXQ_BYTES) { droppedFrames++; return false; }   // full > drop

    Frame *f = (Frame *)malloc(sizeof(Frame) + len);
    if (!f) { droppedFrames++; return false; }
    f->type = type;
    f->len  = len;
    if (len) memcpy(f->data, data, len);

    if (xQueueSend(txq, &f, 0) != pdTRUE) { free(f); droppedFrames++; return false; }
    __atomic_add_fetch(&queuedBytes, len, __ATOMIC_RELAXED);
    return true;
}

static void txTask(void *) {
    static uint8_t enc[2 * OI_MAX_BODY + 2];
    for (;;) {
        Frame *f;
        if (xQueueReceive(txq, &f, portMAX_DELAY) != pdTRUE) continue;

        uint16_t crc = oi_crc16(&f->type, 1);
        crc = oi_crc16(f->data, f->len, crc);

        size_t n = 0;
        auto put = [&](uint8_t b) {
            if (b == OI_SLIP_END)      { enc[n++] = OI_SLIP_ESC; enc[n++] = OI_SLIP_ESC_END; }
            else if (b == OI_SLIP_ESC) { enc[n++] = OI_SLIP_ESC; enc[n++] = OI_SLIP_ESC_ESC; }
            else                         enc[n++] = b;
        };
            enc[n++] = OI_SLIP_END;
            put(f->type);
            for (uint16_t i = 0; i < f->len; i++) put(f->data[i]);
            put(crc >> 8);
        put(crc & 0xFF);
        enc[n++] = OI_SLIP_END;

        Serial.write(enc, n);
        __atomic_add_fetch(&uartTxBytes, (uint32_t)n, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&queuedBytes, f->len, __ATOMIC_RELAXED);
        free(f);
    }
}

// wifi > uart
static esp_err_t wifiRx(void *buf, uint16_t len, void *eb) {
    if (len <= OI_MAX_ETH) enqueue(OI_T_DATA, (const uint8_t *)buf, len);
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static void sendHello() {
    uint8_t p[8];
    p[0] = OI_PROTO_VERSION;
    memcpy(p + 1, staMac, 6);
    p[7] = wifiUp ? 1 : 0;
    enqueue(OI_T_HELLO, p, sizeof(p));
}

// diagnostics
static void sendStats() {
    uint32_t v[5] = { droppedFrames, wifiTxFail, uartTxBytes, uartRxBytes, (uint32_t)Serial.baudRate() };
    uint8_t p[20];
    for (int i = 0; i < 5; i++) {
        p[i * 4]     = v[i] >> 24;
        p[i * 4 + 1] = v[i] >> 16;
        p[i * 4 + 2] = v[i] >> 8;
        p[i * 4 + 3] = v[i];
    }
    enqueue(OI_T_STATS, p, sizeof(p));
}

// uart > wifi
static uint8_t rxbuf[OI_MAX_BODY];
static size_t  rxn = 0;
static bool    rxEsc = false, rxBad = false;
static bool    gotHost = false;
static uint32_t lastHostMs = 0;

static void handleBody(const uint8_t *b, size_t n) {
    uint16_t got = ((uint16_t)b[n - 2] << 8) | b[n - 1];
    if (oi_crc16(b, n - 2) != got) return;

    gotHost = true;
    lastHostMs = millis();

    const uint8_t *payload = b + 1;
    size_t plen = n - 3;

    switch (b[0]) {
        case OI_T_DATA:
            if (wifiUp && plen >= 14) {
                if (esp_wifi_internal_tx(WIFI_IF_STA, (void *)payload, (uint16_t)plen) != ESP_OK)
                    wifiTxFail++;
            }
            break;
        case OI_T_PING:
            sendHello();
            break;
        case OI_T_EXIT:
            Serial.flush();
            ESP.restart();
            break;
    }
}

static void feed(uint8_t c) {
    if (c == OI_SLIP_END) {
        if (!rxBad && rxn >= 3) handleBody(rxbuf, rxn);
        rxn = 0; rxEsc = false; rxBad = false;
        return;
    }
    if (rxBad) return;
    if (c == OI_SLIP_ESC) { rxEsc = true; return; }
    if (rxEsc) {
        rxEsc = false;
        if (c == OI_SLIP_ESC_END)      c = OI_SLIP_END;
        else if (c == OI_SLIP_ESC_ESC) c = OI_SLIP_ESC;
        else { rxBad = true; return; }
    }
    if (rxn >= sizeof(rxbuf)) { rxBad = true; return; }
    rxbuf[rxn++] = c;
}

void bridgeRun(uint32_t baud, const String &ssid, const String &pass) {
    esp_log_level_set("*", ESP_LOG_NONE);   // stray log text would corrupt frames

    // rejoin the network with a static link local address so the esp's own dhcp
    WiFi.disconnect(true, false);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.macAddress(staMac);
    WiFi.config(IPAddress(169, 254, 77, 1), IPAddress(169, 254, 77, 1), IPAddress(255, 255, 0, 0));
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());

    //switch the uart
    Serial.flush();
    Serial.end();
    Serial.setRxBufferSize(8192);
    Serial.begin(baud);

    txq = xQueueCreate(OI_TXQ_FRAMES, sizeof(Frame *));
    xTaskCreatePinnedToCore(txTask, "oi_tx", 4096, nullptr, 5, nullptr, 0);

    uint32_t startMs = millis(), lastTick = 0;
    bool lastUp = false;
    lastHostMs = millis();

    for (;;) {
        checkBootButton();

        int avail = Serial.available();
        if (avail > 0) {
            uint8_t tmp[256];
            int n = Serial.readBytes(tmp, min(avail, (int)sizeof(tmp)));
            uartRxBytes += n;
            for (int i = 0; i < n; i++) feed(tmp[i]);
        } else {
            vTaskDelay(1);
        }

        uint32_t now = millis();
        if (now - lastTick >= 250) {
            lastTick = now;

            wifiUp = (WiFi.status() == WL_CONNECTED);
            if (wifiUp) {
                //reclaim the wifi rx path from lwip
                esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifiRx);
            }
            // announce until the host answer and immediately on link change
            if (!gotHost || wifiUp != lastUp) sendHello();
            lastUp = wifiUp;

            static uint32_t lastStats = 0;
            if (gotHost && now - lastStats >= 1000) { lastStats = now; sendStats(); }

            if (gotHost && now - lastHostMs > 10000) ESP.restart();   // host vanished
            if (!gotHost && now - startMs > 60000)   ESP.restart();   // nobody ever came
        }
    }
}
