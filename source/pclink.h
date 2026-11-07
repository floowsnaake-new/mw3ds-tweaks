// MW3DS PC Link - 3DS side. Non-blocking TCP client talking to PCLink.py.
// Everything is polled from the main thread: no extra threads, no locks.
#pragma once
#include <3ds.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pclink_cmd_cb)(const char* command);

void pclink_init(void);
void pclink_exit(void);
void pclink_update(void);

bool pclink_connect(const char* ip, uint16_t port);
bool pclink_listen(uint16_t port);
void pclink_disconnect(void);
bool pclink_connected(void);
const char* pclink_status(void);
const char* pclink_local_address(void);

const char* pclink_get_address(void);
void pclink_set_address(const char* text);

void pclink_log(const char* fmt, ...);

bool pclink_wants_frame(void);
void pclink_send_frame(const uint16_t* rgb565, int w, int h);

void pclink_apply_input(u32* held, circlePosition* circlePad, circlePosition* cStick);
void pclink_set_command_callback(pclink_cmd_cb cb);
bool pclink_upload_file(const char* path);

// ---- Player state ('M' message) ----
// The web UI sends the "pclink state" console command; main.cpp's
// pclinkRunCommand handles it, fills the geometry buffer, and calls
// pclink_send_player_state(). No other code path emits an 'M' message.
struct PclinkPlayerState {
    float x, y, z;
    float yaw, pitch;
    int   cell;
    int   cell_loaded;
    char  cell_name[64];
    float health, health_max;
    float magicka, magicka_max;
    float fatigue, fatigue_max;
    float armor;
    uint16_t geom_count;
    struct { float x, y; uint8_t kind; } geom[96];
};

extern struct PclinkPlayerState g_pclink_player_state;
void pclink_set_player_state(const float feet[3], float yaw, float pitch,
                             int cell, int live, const char* cell_name,
                             float hp, float hpmax,
                             float mp, float mpmax,
                             float fp, float fpmax,
                             float armor);
void pclink_add_geom(float x, float y, uint8_t kind);
void pclink_send_player_state(void);

#ifdef __cplusplus
}
#endif
