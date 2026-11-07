#include "pclink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

/* the game's logf (source/log.h): C++ mangled, so C code calls pclink_logline (source/log.cpp) */
extern void pclink_logline(const char* line);
static void plog(const char* fmt, ...)
{
    char b[200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    pclink_logline(b);
}

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#define PCLINK_DEFAULT_PORT 50763
#define SOC_BUF_SIZE        0x60000
#define RX_MAX              2048

enum { ST_OFF, ST_CONNECTING, ST_LISTEN, ST_UP };

static int  g_state = ST_OFF, g_fd = -1, g_listen_fd = -1;
static u32* g_soc_buf = NULL;
static char g_status[128] = "PC link off";
static char g_addr[32]   = "no wifi";
static pclink_cmd_cb g_cb = NULL;
static char g_saved_addr[64] = "";

static u32  g_keys = 0;
static s16  g_cp[2], g_cs[2];
static u64  g_input_ms = 0;
static u32  g_fps_milli = 0;
static u64  g_last_frame_ms = 0, g_conn_ms = 0;
static uint8_t  g_rx[RX_MAX];
static size_t   g_rxn = 0;
static uint16_t g_small[200 * 120];

#define TX_MAX (5 + 4 + 200 * 120 * 2 + 16)
static uint8_t g_tx[TX_MAX];
static size_t  g_txn = 0, g_txoff = 0;
static u64     g_tx_progress_ms = 0;

/* Player snapshot (filled by main.cpp on demand, sent when it calls pclink_send_player_state) */
struct PclinkPlayerState g_pclink_player_state;

static void set_status(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
}

static void drop(const char* why)
{
    plog("pclink: drop: %s", why);
    if (g_fd >= 0) {
        uint8_t tmp[256];
        for (int i = 0; i < 32; i++) {
            ssize_t n = recv(g_fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
        }
        shutdown(g_fd, SHUT_RDWR);
        close(g_fd);
        g_fd = -1;
    }
    g_state = (g_listen_fd >= 0) ? ST_LISTEN : ST_OFF;
    g_rxn = 0; g_keys = 0; g_fps_milli = 0; g_txn = g_txoff = 0;
    g_cp[0] = g_cp[1] = g_cs[0] = g_cs[1] = 0;
    if (g_listen_fd >= 0) set_status("Listening - waiting for PC...");
    else set_status("%s", why);
}

static void tx_pump(void)
{
    if (g_state != ST_UP || g_txn == 0) return;
    while (g_txoff < g_txn) {
        ssize_t n = send(g_fd, g_tx + g_txoff, g_txn - g_txoff, 0);
        if (n > 0) { g_txoff += (size_t)n; g_tx_progress_ms = osGetTime(); continue; }
        if (n == 0 || errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOTCONN ||
            errno == EINPROGRESS || errno == EALREADY || errno == ECONNRESET) {
            if (osGetTime() - g_tx_progress_ms > 10000)
                drop("Link lost (send stalled)");
            return;
        }
        char why[64];
        snprintf(why, sizeof why, "Link lost (send failed, errno %d)", errno);
        drop(why);
        return;
    }
    g_txn = g_txoff = 0;
}

static void send_msg(char type, const void* pre, size_t prelen,
                     const void* body, size_t bodylen, int timeout_ms)
{
    (void)timeout_ms;
    if (g_state != ST_UP) return;
    if (g_txn) {
        tx_pump();
        if (g_state != ST_UP || g_txn) return;
    }
    size_t total = prelen + bodylen;
    if (5 + total > TX_MAX) return;
    uint32_t t32 = (uint32_t)total;
    g_tx[0] = (uint8_t)type;
    memcpy(g_tx + 1, &t32, 4);
    if (prelen)  memcpy(g_tx + 5, pre, prelen);
    if (bodylen) memcpy(g_tx + 5 + prelen, body, bodylen);
    g_txn = 5 + total; g_txoff = 0; g_tx_progress_ms = osGetTime();
    tx_pump();
}

/* ---- Save export ---- */
#define PCLINK_UPLOAD_CHUNK 16384

static bool upload_wait_drain(int timeout_ms)
{
    int waited = 0;
    while (g_txn != 0) {
        if (g_state != ST_UP)
            return false;
        tx_pump();
        svcSleepThread(1000000LL);
        if (++waited > timeout_ms)
            return false;
    }
    return true;
}

bool pclink_upload_file(const char* path)
{
    if (g_state != ST_UP) {
        plog("pclink: upload refused, not linked");
        return false;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        plog("pclink: cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        plog("pclink: %s is %ld bytes, refusing", path, size);
        fclose(f);
        return false;
    }
    const char* name = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            name = p + 1;
    size_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > 255) {
        fclose(f);
        return false;
    }
    uint8_t hdr[2 + 255 + 4];
    uint16_t nl = (uint16_t)nameLen;
    uint32_t sz = (uint32_t)size;
    memcpy(hdr, &nl, 2);
    memcpy(hdr + 2, name, nameLen);
    memcpy(hdr + 2 + nameLen, &sz, 4);
    if (!upload_wait_drain(2000)) {
        fclose(f);
        return false;
    }
    send_msg('S', NULL, 0, hdr, 2 + nameLen + 4, 2000);

    uint8_t chunk[PCLINK_UPLOAD_CHUNK];
    long sent = 0;
    bool ok = true;
    while (sent < size) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n == 0) { ok = false; break; }
        if (!upload_wait_drain(5000)) { ok = false; break; }
        send_msg('S', NULL, 0, chunk, n, 5000);
        sent += (long)n;
    }
    fclose(f);
    if (ok)
        plog("pclink: exported %s (%ld bytes)", name, size);
    else
        plog("pclink: export of %s stopped after %ld of %ld bytes", name, sent, size);
    return ok;
}

/* ---- Player state ('M' message) ---- */

void pclink_set_player_state(const float feet[3], float yaw, float pitch,
                             int cell, int live, const char* cell_name,
                             float hp, float hpmax,
                             float mp, float mpmax,
                             float fp, float fpmax,
                             float armor)
{
    g_pclink_player_state.x = feet[0];
    g_pclink_player_state.y = feet[1];
    g_pclink_player_state.z = feet[2];
    g_pclink_player_state.yaw = yaw;
    g_pclink_player_state.pitch = pitch;
    g_pclink_player_state.cell = cell;
    g_pclink_player_state.cell_loaded = live;
    if (cell_name) {
        strncpy(g_pclink_player_state.cell_name, cell_name,
                sizeof(g_pclink_player_state.cell_name) - 1);
        g_pclink_player_state.cell_name[sizeof(g_pclink_player_state.cell_name) - 1] = 0;
    } else {
        g_pclink_player_state.cell_name[0] = 0;
    }
    g_pclink_player_state.health = hp;
    g_pclink_player_state.health_max = hpmax;
    g_pclink_player_state.magicka = mp;
    g_pclink_player_state.magicka_max = mpmax;
    g_pclink_player_state.fatigue = fp;
    g_pclink_player_state.fatigue_max = fpmax;
    g_pclink_player_state.armor = armor;
}

void pclink_add_geom(float x, float y, uint8_t kind)
{
    if (g_pclink_player_state.geom_count < 96) {
        g_pclink_player_state.geom[g_pclink_player_state.geom_count].x = x;
        g_pclink_player_state.geom[g_pclink_player_state.geom_count].y = y;
        g_pclink_player_state.geom[g_pclink_player_state.geom_count].kind = kind;
        g_pclink_player_state.geom_count++;
    }
}

void pclink_send_player_state(void)
{
    if (g_state != ST_UP) return;
    uint8_t buf[1024];
    uint8_t* p = buf;
    #define PUT(f, v) do { memcpy(p, &(v), sizeof(f)); p += sizeof(f); } while (0)
    PUT(float, g_pclink_player_state.x);
    PUT(float, g_pclink_player_state.y);
    PUT(float, g_pclink_player_state.z);
    PUT(float, g_pclink_player_state.yaw);
    PUT(float, g_pclink_player_state.pitch);
    PUT(int,   g_pclink_player_state.cell);
    PUT(int,   g_pclink_player_state.cell_loaded);
    memcpy(p, g_pclink_player_state.cell_name, 64); p += 64;
    PUT(float, g_pclink_player_state.health);
    PUT(float, g_pclink_player_state.health_max);
    PUT(float, g_pclink_player_state.magicka);
    PUT(float, g_pclink_player_state.magicka_max);
    PUT(float, g_pclink_player_state.fatigue);
    PUT(float, g_pclink_player_state.fatigue_max);
    PUT(float, g_pclink_player_state.armor);
    PUT(uint16_t, g_pclink_player_state.geom_count);
    for (uint16_t i = 0; i < g_pclink_player_state.geom_count; i++) {
        PUT(float, g_pclink_player_state.geom[i].x);
        PUT(float, g_pclink_player_state.geom[i].y);
        PUT(uint8_t, g_pclink_player_state.geom[i].kind);
    }
    #undef PUT
    send_msg('M', NULL, 0, buf, (size_t)(p - buf), 100);
}

/* ---- Incoming messages ---- */

static void handle(uint8_t t, const uint8_t* d, uint32_t n)
{
    if (t == 'I' && n >= 12) {
        s16 v[4];
        memcpy(&g_keys, d, 4); memcpy(v, d + 4, 8);
        g_cp[0] = v[0]; g_cp[1] = v[1]; g_cs[0] = v[2]; g_cs[1] = v[3];
        g_input_ms = osGetTime();
    } else if (t == 'C' && n > 0 && n < 256) {
        char cmd[256]; memcpy(cmd, d, n); cmd[n] = 0;
        if (g_cb) g_cb(cmd);
    } else if (t == 'R' && n >= 2) {
        uint16_t m; memcpy(&m, d, 2); g_fps_milli = m;
    }
}

void pclink_init(void)
{
#ifndef PCLINK_SKIP_SOCINIT
    if (g_soc_buf) return;
    g_soc_buf = (u32*)memalign(0x1000, SOC_BUF_SIZE);
    if (!g_soc_buf || R_FAILED(socInit(g_soc_buf, SOC_BUF_SIZE))) {
        free(g_soc_buf); g_soc_buf = NULL;
        plog("pclink: socInit failed (SOC_BUF_SIZE=0x%x)", SOC_BUF_SIZE);
    } else {
        plog("pclink: socInit ok (SOC_BUF_SIZE=0x%x)", SOC_BUF_SIZE);
    }
#endif
}

void pclink_exit(void)
{
    pclink_disconnect();
#ifndef PCLINK_SKIP_SOCINIT
    if (g_soc_buf) { socExit(); free(g_soc_buf); g_soc_buf = NULL; }
#endif
}

bool pclink_listen(uint16_t port)
{
    if (g_state != ST_OFF) pclink_disconnect();
    if (!port) port = PCLINK_DEFAULT_PORT;
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { set_status("No network (is Wi-Fi on?)"); return false; }
    int yes = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        close(g_listen_fd); g_listen_fd = -1;
        set_status("Bind failed (%d)", errno); return false;
    }
    if (listen(g_listen_fd, 1) < 0) {
        close(g_listen_fd); g_listen_fd = -1;
        set_status("Listen failed (%d)", errno); return false;
    }
    g_state = ST_LISTEN;
    set_status("Listening on :%u - connect from PC", (unsigned)port);
    return true;
}

bool pclink_connect(const char* ip, uint16_t port)
{
    char buf[64] = {0};
    if (g_state == ST_CONNECTING) return true;
    if (g_state != ST_OFF) pclink_disconnect();
    if (!ip) {
        snprintf(buf, sizeof buf, "%s", g_saved_addr);
        buf[strcspn(buf, "\r\n ")] = 0;
        if (!buf[0]) { set_status("Tap 'PC address' and type the PC IP first"); return false; }
        if (!strncmp(buf, "listen", 6)) {
            uint16_t lp = PCLINK_DEFAULT_PORT;
            char* colon = strchr(buf, ':');
            if (colon) lp = (uint16_t)atoi(colon + 1);
            if (!lp) lp = PCLINK_DEFAULT_PORT;
            return pclink_listen(lp);
        }
        char* colon = strchr(buf, ':');
        if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
        ip = buf;
    }
    if (!port) port = PCLINK_DEFAULT_PORT;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (!inet_aton(ip, &dst.sin_addr)) { set_status("Bad PC address"); return false; }
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) { set_status("No network (is Wi-Fi on?)"); return false; }
    { int yes = 1; setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes); }
    fcntl(g_fd, F_SETFL, fcntl(g_fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(g_fd, (struct sockaddr*)&dst, sizeof dst) < 0 && errno != EINPROGRESS) {
        close(g_fd); g_fd = -1;
        set_status("Connect failed (%d)", errno); return false;
    }
    g_state = ST_CONNECTING;
    g_conn_ms = osGetTime();
    set_status("Connecting to %s:%u...", ip, (unsigned)port);
    return true;
}

void pclink_disconnect(void)
{
    if (g_fd >= 0) {
        uint8_t tmp[256];
        for (int i = 0; i < 32; i++) {
            ssize_t n = recv(g_fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
        }
        shutdown(g_fd, SHUT_RDWR);
        close(g_fd); g_fd = -1;
    }
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    g_state = ST_OFF; g_rxn = 0; g_keys = 0; g_fps_milli = 0; g_txn = g_txoff = 0;
    g_cp[0] = g_cp[1] = g_cs[0] = g_cs[1] = 0;
    set_status("PC link off");
}

bool pclink_connected(void) { return g_state == ST_UP; }
const char* pclink_status(void) { return g_status; }

const char* pclink_local_address(void)
{
    struct in_addr a;
    a.s_addr = (in_addr_t)gethostid();
    if (!a.s_addr) snprintf(g_addr, sizeof g_addr, "no wifi");
    else snprintf(g_addr, sizeof g_addr, "%s", inet_ntoa(a));
    return g_addr;
}

const char* pclink_get_address(void) { return g_saved_addr; }

void pclink_set_address(const char* text)
{
    if (!text) { g_saved_addr[0] = 0; return; }
    snprintf(g_saved_addr, sizeof g_saved_addr, "%s", text);
    g_saved_addr[strcspn(g_saved_addr, "\r\n ")] = 0;
    set_status("PC address: %s", g_saved_addr[0] ? g_saved_addr : "(none)");
}

void pclink_update(void)
{
    if (g_state == ST_OFF) return;
    if (g_state == ST_LISTEN) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int c = accept(g_listen_fd, (struct sockaddr*)&peer, &plen);
        if (c >= 0) {
            fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
            int yes = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
            if (g_fd >= 0) close(g_fd);
            g_fd = c; g_state = ST_UP; g_rxn = 0;
            set_status("Linked (PC connected in)");
            const char hello[] = "MW3DS PC link ready (listen mode)";
            send_msg('H', NULL, 0, hello, sizeof hello - 1, 200);
        }
        return;
    }
    if (g_state == ST_CONNECTING) {
        struct pollfd pf = { g_fd, POLLOUT, 0 };
        if (poll(&pf, 1, 0) > 0) {
            int err = 0; socklen_t l = sizeof err;
            getsockopt(g_fd, SOL_SOCKET, SO_ERROR, &err, &l);
            if (err > 0) {
                char why[64];
                snprintf(why, sizeof why, "PC refused (errno %d)", err);
                drop(why);
            } else {
                g_state = ST_UP;
                plog("pclink: connected (outgoing), sending hello");
                set_status("Linked to PC");
                svcSleepThread(30000000LL);
                const char hello[] = "MW3DS PC link ready";
                send_msg('H', NULL, 0, hello, sizeof hello - 1, 1000);
            }
        } else if (osGetTime() - g_conn_ms > 6000) {
            drop("PC connect timed out");
        }
        return;
    }
    tx_pump();
    for (int i = 0; i < 8 && g_state == ST_UP; i++) {
        ssize_t n = recv(g_fd, g_rx + g_rxn, RX_MAX - g_rxn, 0);
        if (n > 0) { g_rxn += (size_t)n; }
        else if (n == 0) { drop("PC closed the link"); return; }
        else if (errno == EWOULDBLOCK || errno == EAGAIN) break;
        else { drop("Link lost (recv failed)"); return; }
        while (g_rxn >= 5) {
            uint32_t len; memcpy(&len, g_rx + 1, 4);
            if (len > RX_MAX - 5) { drop("Protocol error"); return; }
            if (g_rxn < 5 + len) break;
            handle(g_rx[0], g_rx + 5, len);
            memmove(g_rx, g_rx + 5 + len, g_rxn - 5 - len);
            g_rxn -= 5 + len;
        }
    }
    if ((g_keys || g_cp[0] || g_cp[1] || g_cs[0] || g_cs[1]) && osGetTime() - g_input_ms > 500) {
        g_keys = 0; g_cp[0] = g_cp[1] = g_cs[0] = g_cs[1] = 0;
    }
}

void pclink_log(const char* fmt, ...)
{
    if (g_state != ST_UP) return;
    char b[200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    send_msg('L', NULL, 0, b, strlen(b), 100);
}

bool pclink_wants_frame(void)
{
    if (g_state != ST_UP || !g_fps_milli) return false;
    if (g_txn) return false;
    if (g_last_frame_ms == 0) return true;
    u64 interval_ms = 1000000ULL / g_fps_milli;
    if (!interval_ms) interval_ms = 1;
    return (osGetTime() - g_last_frame_ms) >= interval_ms;
}

void pclink_send_frame(const uint16_t* px, int w, int h)
{
    if (g_state != ST_UP || !px || w <= 0 || h <= 0) return;
    struct pollfd pf = { g_fd, POLLOUT, 0 };
    if (poll(&pf, 1, 0) <= 0) return;
    g_last_frame_ms = osGetTime();
    int step = (w >= 400) ? 2 : 1;
    int sw = w / step, sh = h / step;
    if (sw * sh > (int)(sizeof g_small / sizeof g_small[0])) return;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            g_small[y * sw + x] = px[(y * step) * w + (x * step)];
    uint16_t dim[2] = { (uint16_t)sw, (uint16_t)sh };
    send_msg('F', dim, 4, g_small, (size_t)sw * sh * 2, 500);
}

void pclink_apply_input(u32* held, circlePosition* cp, circlePosition* cs)
{
    if (g_state != ST_UP) return;
    if (held) *held |= g_keys;
    if (cp && (g_cp[0] || g_cp[1])) { cp->dx = g_cp[0]; cp->dy = g_cp[1]; }
    if (cs && (g_cs[0] || g_cs[1])) { cs->dx = g_cs[0]; cs->dy = g_cs[1]; }
}

void pclink_set_command_callback(pclink_cmd_cb cb) { g_cb = cb; }
