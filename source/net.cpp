#include "net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#include "log.h"

NetEngine::NetEngine() {}
NetEngine::~NetEngine() { disconnect(); }

static void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static void setNoDelay(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

int NetEngine::findFreeSlot() {
    for (int i = 0; i < kMaxPeers; i++)
        if (peers_[i].sock < 0)
            return i;
    return -1;
}

bool NetEngine::host(uint16_t port) {
    if (listenSock_ >= 0) disconnect();
    listenSock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock_ < 0) { logf("net: socket() failed (%d)", errno); return false; }
    int yes = 1;
    setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        logf("net: bind(%d) failed (%d)", port, errno);
        close(listenSock_); listenSock_ = -1; return false;
    }
    if (listen(listenSock_, kMaxPeers) < 0) {
        logf("net: listen() failed (%d)", errno);
        close(listenSock_); listenSock_ = -1; return false;
    }
    setNonBlocking(listenSock_);
    host_ = true;
    logf("net: hosting on port %d, up to %d players", port, kMaxPeers);
    return true;
}

bool NetEngine::join(const char* hostIp, uint16_t port) {
    if (listenSock_ >= 0 || connectedCount_ > 0) disconnect();
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { logf("net: socket() failed (%d)", errno); return false; }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(hostIp, &addr.sin_addr) == 0) {
        logf("net: bad IP '%s'", hostIp);
        close(s); return false;
    }
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        logf("net: connect(%s:%d) failed (%d)", hostIp, port, errno);
        close(s); return false;
    }
    setNonBlocking(s);
    setNoDelay(s);
    host_ = false;
    peers_[0].sock = s;
    peers_[0].connected = true;
    connectedCount_ = 1;
    logf("net: connected to %s:%d", hostIp, port);
    return true;
}

void NetEngine::closePeer(Peer& p) {
    if (p.sock >= 0) { shutdown(p.sock, SHUT_RDWR); close(p.sock); p.sock = -1; }
    if (p.connected) { p.connected = false; if (connectedCount_ > 0) connectedCount_--; }
    p.rx.clear();
}

void NetEngine::disconnect() {
    for (auto& p : peers_) closePeer(p);
    if (listenSock_ >= 0) { close(listenSock_); listenSock_ = -1; }
    host_ = false;
    connectedCount_ = 0;
    logf("net: disconnected");
}

void NetEngine::acceptIfPending() {
    if (listenSock_ < 0) return;
    for (;;) {
        sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(listenSock_, (sockaddr*)&peer, &plen);
        if (c < 0) break;
        int slot = findFreeSlot();
        if (slot < 0) { logf("net: full, refusing %s", inet_ntoa(peer.sin_addr)); close(c); continue; }
        setNonBlocking(c);
        setNoDelay(c);
        peers_[slot].sock = c;
        peers_[slot].connected = true;
        connectedCount_++;
        logf("net: peer %d connected from %s", slot, inet_ntoa(peer.sin_addr));
    }
}

void NetEngine::flushTx(int i) {
    Peer& p = peers_[i];
    while (p.sock >= 0 && !p.tx.empty()) {
        ssize_t n = ::send(p.sock, p.tx.data(), p.tx.size(), 0);
        if (n > 0) { p.tx.erase(p.tx.begin(), p.tx.begin() + n); continue; }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) break;
        logf("net: peer %d send failed (%d)", i, errno);
        closePeer(p);
        if (handler_) handler_(i, 0xFF, nullptr, 0, user_);
        break;
    }
}

void NetEngine::poll() {
    acceptIfPending();
    /* MPFIX_D: flush outgoing queues */
    for (int i = 0; i < kMaxPeers; i++) flushTx(i);
    for (int i = 0; i < kMaxPeers; i++) {
        Peer& p = peers_[i];
        if (p.sock < 0) continue;
        uint8_t buf[4096];
        for (int iter = 0; iter < 4; iter++) {
            ssize_t n = recv(p.sock, buf, sizeof(buf), 0);
            if (n > 0) p.rx.insert(p.rx.end(), buf, buf + n);
            else if (n == 0) {
                logf("net: peer %d closed", i);
                closePeer(p);
                if (handler_) handler_(i, 0xFF, nullptr, 0, user_);
                break;
            } else {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    logf("net: peer %d recv failed (%d)", i, errno);
                    closePeer(p);
                    if (handler_) handler_(i, 0xFF, nullptr, 0, user_);
                }
                break;
            }
        }
        while (p.sock >= 0 && p.rx.size() >= 5) {
            uint8_t tag = p.rx[0];
            uint32_t len;
            memcpy(&len, &p.rx[1], 4);
            if (len > 65536) {
                logf("net: peer %d msg too long", i);
                closePeer(p);
                if (handler_) handler_(i, 0xFF, nullptr, 0, user_);
                break;
            }
            if (p.rx.size() < 5 + len) break;
            if (handler_) handler_(i, tag, &p.rx[5], len, user_);
            p.rx.erase(p.rx.begin(), p.rx.begin() + 5 + len);
        }
    }
}

bool NetEngine::sendMsgTo(int peer, uint8_t tag, const void* data, uint32_t len) {
    // MPFIX_D: queue whole message
    if (peer < 0 || peer >= kMaxPeers) return false;
    Peer& p = peers_[peer];
    if (p.sock < 0 || !p.connected) return false;
    // One frame (tag + length + payload) goes out whole or not at all. Splitting
    // header and payload on a non-blocking socket could desync the stream.
    const size_t kMaxQueued = 256 * 1024;
    if (p.tx.size() + 5 + len > kMaxQueued) return false;
    size_t off = p.tx.size();
    p.tx.resize(off + 5 + len);
    p.tx[off] = tag;
    memcpy(&p.tx[off + 1], &len, 4);
    if (len > 0) memcpy(&p.tx[off + 5], data, len);
    flushTx(peer);
    return true;
}

bool NetEngine::broadcastMsg(uint8_t tag, const void* data, uint32_t len) {
    bool any = false;
    for (int i = 0; i < kMaxPeers; i++)
        if (peers_[i].sock >= 0 && peers_[i].connected)
            any |= sendMsgTo(i, tag, data, len);
    return any;
}
