#pragma once
#include <3ds.h>
#include <cstdint>
#include <string>
#include <vector>

enum NetMsg : uint8_t {
    NM_HELLO = 0x01,
    NM_WELCOME = 0x02,
    NM_CHAT = 0x50,
    NM_PLAYERSTATE = 0x70,
    NM_PING = 0x60,
    NM_PONG = 0x61,
    NM_LEAVE = 0x62,
    NM_TRADE = 0x80,
    NM_NPCSTATE = 0x90,
    NM_WEATHER = 0x91,
    NM_WORLDEVENT = 0x92,
    NM_ITEMSYNC = 0x93,   /* MPITEMS: world item + container state */
    NM_SNAPSHOT = 0x94,   /* late-join: host finished (or started) world dump */
    NM_COMBAT = 0x95,     /* host-auth NPC/player damage + death */
    NM_PROJECTILE = 0x96, /* arrow / spell bolt spawn */
    NM_CELL = 0x97,       /* MPCELL: which cell a peer is in (+ where), so both can meet in one cell */
    NM_COMPANION = 0x98,  /* host companion gear / dressable body (race, worn, weaponOut) */
};

class NetEngine {
public:
    static constexpr int kMaxPeers = 4;

    NetEngine();
    ~NetEngine();

    bool host(uint16_t port);
    bool join(const char* host, uint16_t port);
    void disconnect();
    void poll();

    bool isHost() const { return host_; }
    bool isClient() const { return !host_ && connectedCount_ > 0; }
    bool isConnected() const { return connectedCount_ > 0; }
    int  peerCount() const { return connectedCount_; }

    bool sendMsgTo(int peer, uint8_t tag, const void* data, uint32_t len);
    bool broadcastMsg(uint8_t tag, const void* data, uint32_t len);
    template <typename T> bool broadcast(uint8_t tag, const T& t) {
        return broadcastMsg(tag, &t, sizeof(t));
    }

    bool sendMsg(uint8_t tag, const void* data, uint32_t len) {
        return sendMsgTo(0, tag, data, len);
    }
    template <typename T> bool send(uint8_t tag, const T& t) {
        return sendMsgTo(0, tag, &t, sizeof(t));
    }

    using Handler = void (*)(int peer, uint8_t tag, const uint8_t* data, uint32_t len, void* user);
    void setHandler(Handler h, void* user) { handler_ = h; user_ = h ? user : nullptr; }

private:
    struct Peer {
        int sock = -1;
        std::vector<uint8_t> rx;
        std::vector<uint8_t> tx;   // MPFIX0: bytes the socket hasn't taken yet

        bool connected = false;
    };
    Peer peers_[kMaxPeers];
    int listenSock_ = -1;
    bool host_ = false;
    int connectedCount_ = 0;
    Handler handler_ = nullptr;
    void* user_ = nullptr;

    void acceptIfPending();
    void closePeer(Peer& p);
    void flushTx(int i);   // MPFIX0
    int findFreeSlot();
};
