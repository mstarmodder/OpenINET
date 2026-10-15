// OpenINET host daemon for FreeBSD: bridges the ESP32 serial link to a tap(4) interface.
// Build: c++ -O2 -std=c++17 -o openinetd_freebsd.cpp -o openinetd
// Run: ./openinetd -d /dev/cuaU0 -b 460800 -i tap0

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../src/proto.h"

static long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static volatile sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

struct Decoder {
    uint8_t buf[OI_MAX_BODY];
    size_t n = 0;
    bool esc = false, bad = false;

    template <class F> void feed(uint8_t b, F &&onFrame) {
        if (b == OI_SLIP_END) {
            if (!bad && n >= 3) {
                uint16_t got = ((uint16_t)buf[n - 2] << 8) | buf[n - 1];
                if (oi_crc16(buf, n - 2) == got) onFrame(buf[0], buf + 1, n - 3);
            }
            n = 0; esc = false; bad = false;
            return;
        }
        if (bad) return;
        if (b == OI_SLIP_ESC) { esc = true; return; }
        if (esc) {
            esc = false;
            if (b == OI_SLIP_ESC_END)      b = OI_SLIP_END;
            else if (b == OI_SLIP_ESC_ESC) b = OI_SLIP_ESC;
            else { bad = true; return; }
        }
        if (n >= sizeof(buf)) { bad = true; return; }
        buf[n++] = b;
    }
};

static void encode(std::vector<uint8_t> &out, uint8_t type, const uint8_t *p, size_t n) {
    uint16_t crc = oi_crc16(&type, 1);
    crc = oi_crc16(p, n, crc);
    auto put = [&](uint8_t b) {
        if (b == OI_SLIP_END)      { out.push_back(OI_SLIP_ESC); out.push_back(OI_SLIP_ESC_END); }
        else if (b == OI_SLIP_ESC) { out.push_back(OI_SLIP_ESC); out.push_back(OI_SLIP_ESC_ESC); }
        else out.push_back(b);
    };
        out.push_back(OI_SLIP_END);
        put(type);
        for (size_t i = 0; i < n; i++) put(p[i]);
        put(crc >> 8);
    put(crc & 0xFF);
    out.push_back(OI_SLIP_END);
}

struct Session {
    int ser = -1, tap = -1;
    std::string ifname;
    Decoder dec;
    std::vector<uint8_t> txq;
    size_t txoff = 0;
    bool gotHello = false, linkUp = false, macSet = false;
    uint8_t mac[6] = {0};
    long lastRx = 0;

    void resetLink() {
        dec = Decoder();
        txq.clear(); txoff = 0;
        gotHello = false;
        lastRx = nowMs();
    }
    size_t pending() const { return txq.size() - txoff; }

    void send(uint8_t type, const uint8_t *p, size_t n) { encode(txq, type, p, n); }

    void flush() {
        while (txoff < txq.size()) {
            ssize_t w = write(ser, &txq[txoff], txq.size() - txoff);
            if (w > 0) txoff += (size_t)w; else break;
        }
        if (txoff == txq.size()) { txq.clear(); txoff = 0; }
    }

    void setMac(const uint8_t *m) {
        if (macSet && memcmp(mac, m, 6) == 0) return;
        memcpy(mac, m, 6);
        macSet = true;
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "ifconfig %s ether %02x:%02x:%02x:%02x:%02x:%02x up",
                 ifname.c_str(), m[0], m[1], m[2], m[3], m[4], m[5]);
        if (system(cmd) != 0) fprintf(stderr, "warning: '%s' failed\n", cmd);
        else printf("%s is up with MAC %02x:%02x:%02x:%02x:%02x:%02x (run: dhclient %s)\n",
            ifname.c_str(), m[0], m[1], m[2], m[3], m[4], m[5], ifname.c_str());
    }

    void onFrame(uint8_t type, const uint8_t *p, size_t n) {
        lastRx = nowMs();
        if (type == OI_T_DATA) {
            if (tap >= 0) (void)!write(tap, p, n);
        } else if (type == OI_T_HELLO && n >= 8 && p[0] == OI_PROTO_VERSION) {
            bool up = p[7] != 0;
            if (!gotHello || up != linkUp) printf("esp32: wifi %s\n", up ? "connected" : "disconnected");
            gotHello = true;
            linkUp = up;
            setMac(p + 1);
        }
    }
};

static bool setBaud(int fd, unsigned baud) {
    termios t;
    if (tcgetattr(fd, &t) != 0) return false;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetspeed(&t, baud);
    return tcsetattr(fd, TCSADRAIN, &t) == 0;
}

static int openSerial(const char *path, unsigned baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (!setBaud(fd, baud)) { close(fd); return -1; }
    int bits = TIOCM_DTR | TIOCM_RTS;      // keep the auto-reset circuit released
    ioctl(fd, TIOCMBIC, &bits);
    return fd;
}

// One poll round. Returns false on a fatal serial error (unplugged, etc.)
static bool step(Session &s, int timeoutMs, bool useTap) {
    pollfd p[2];
    p[0] = {s.ser, (short)(POLLIN | (s.pending() ? POLLOUT : 0)), 0};
    nfds_t nf = 1;
    if (useTap) { p[1] = {s.tap, POLLIN, 0}; nf = 2; }

    int r = poll(p, nf, timeoutMs);
    if (r < 0) return errno == EINTR;
    if (r == 0) return true;

    if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return false;

    if (p[0].revents & POLLIN) {
        uint8_t b[2048];
        ssize_t n = read(s.ser, b, sizeof(b));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                s.dec.feed(b[i], [&](uint8_t t, const uint8_t *pl, size_t l) { s.onFrame(t, pl, l); });
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return false;
        }
    }
    if (p[0].revents & POLLOUT) s.flush();

    if (useTap && (p[1].revents & POLLIN)) {
        static uint8_t fr[4096];
        for (int i = 0; i < 16; i++) {
            ssize_t n = read(s.tap, fr, sizeof(fr));
            if (n <= 0) break;
            if ((size_t)n <= OI_MAX_ETH && s.pending() < 32768) s.send(OI_T_DATA, fr, (size_t)n);
        }
        s.flush();
    }
    return true;
}

static bool waitHello(Session &s, int totalMs, bool ping) {
    long end = nowMs() + totalMs, lastPing = 0;
    while (!g_stop && nowMs() < end) {
        if (ping && nowMs() - lastPing >= 300) {
            s.send(OI_T_PING, nullptr, 0);
            s.flush();
            lastPing = nowMs();
        }
        if (!step(s, 100, false)) return false;
        if (s.gotHello) return true;
    }
    return false;
}

static void writeAll(int fd, const char *str) {
    (void)!write(fd, str, strlen(str));
    tcdrain(fd);
}

static bool connectEsp(Session &s, const char *dev, unsigned baud) {
    s.ser = openSerial(dev, baud);
    if (s.ser < 0) { perror(dev); return false; }
    s.resetLink();

    usleep(1500 * 1000);              // opening the port may reset the board
    tcflush(s.ser, TCIOFLUSH);

    // already in bridge mode (daemon restarted without a board reset)?
    if (waitHello(s, 1500, true)) return true;

    // otherwise drive the shell: start bridge mode, then follow at the new baud
    printf("starting bridge mode on the dongle...\n");
    setBaud(s.ser, 115200);
    tcflush(s.ser, TCIOFLUSH);
    writeAll(s.ser, "\r\n");
    usleep(200 * 1000);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "netstart %u\r\n", baud);
    writeAll(s.ser, cmd);
    usleep(700 * 1000);
    setBaud(s.ser, baud);
    tcflush(s.ser, TCIFLUSH);
    s.resetLink();

    if (waitHello(s, 8000, true)) return true;

    fprintf(stderr, "no answer from the dongle (is a network saved?)\n");
    close(s.ser);
    s.ser = -1;
    return false;
}

static bool validIfName(const std::string &n) {
    if (n.rfind("tap", 0) != 0 || n.size() < 4 || n.size() > 8) return false;
    for (size_t i = 3; i < n.size(); i++) if (!isdigit((unsigned char)n[i])) return false;
    return true;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/cuaU0";
    unsigned baud = 460800;
    std::string ifname = "tap0";

    int o;
    while ((o = getopt(argc, argv, "d:b:i:")) != -1) {
        if (o == 'd') dev = optarg;
        else if (o == 'b') baud = (unsigned)atoi(optarg);
        else if (o == 'i') ifname = optarg;
        else { fprintf(stderr, "usage: %s [-d /dev/cuaU0] [-b 460800] [-i tap0]\n", argv[0]); return 2; }
    }
    if (!validIfName(ifname) || baud < 115200 || baud > 921600) {
        fprintf(stderr, "bad interface name or baud (115200..921600)\n");
        return 2;
    }

    sigaction_fix:
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = onSignal;          // no SA_RESTART: poll() must wake up
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    Session s;
    s.ifname = ifname;

    (void)!system(("ifconfig " + ifname + " create 2>/dev/null").c_str());
    s.tap = open(("/dev/" + ifname).c_str(), O_RDWR | O_NONBLOCK);
    if (s.tap < 0) {
        perror(("/dev/" + ifname).c_str());
        fprintf(stderr, "need root and the tap driver (kldload if_tap)\n");
        return 1;
    }

    while (!g_stop) {
        if (!connectEsp(s, dev, baud)) { sleep(2); continue; }
        printf("bridge running (Ctrl-C to stop and return the esp32 to its shell)\n");

        long lastPing = nowMs();
        while (!g_stop) {
            if (!step(s, 250, true)) { fprintf(stderr, "serial error\n"); break; }
            long now = nowMs();
            if (now - lastPing >= 2000) {
                s.send(OI_T_PING, nullptr, 0);
                s.flush();
                lastPing = now;
            }
            if (now - s.lastRx > 8000) { fprintf(stderr, "esp32 stopped responding\n"); break; }
        }
        if (g_stop) {
            s.send(OI_T_EXIT, nullptr, 0);
            for (int i = 0; i < 20 && s.pending(); i++) { s.flush(); usleep(10000); }
            tcdrain(s.ser);
        }
        close(s.ser);
        s.ser = -1;
    }
    return 0;
}
