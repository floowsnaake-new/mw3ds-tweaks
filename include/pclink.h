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

void pclink_init(void);      // once at startup (initialises sockets unless PCLINK_SKIP_SOCINIT)
void pclink_exit(void);      // once at shutdown
void pclink_update(void);    // once per frame: connect progress, receive PC messages

// ip == NULL -> read "ip" or "ip:port" from sdmc:/mw3ds/pclink.txt. port 0 -> 50763.
bool pclink_connect(const char* ip, uint16_t port);
// Listen mode: PC connects TO the 3DS. port 0 -> 50763.
bool pclink_listen(uint16_t port);
void pclink_disconnect(void);
bool pclink_connected(void);
const char* pclink_status(void);        // short text for the menu
const char* pclink_local_address(void); // this 3DS's IP ("Show 3DS address")

// The PC address typed on the 3DS's on-screen keyboard. Read at start
// from sdmc:/mw3ds/pclink.txt; written back when set.
const char* pclink_get_address(void);
void pclink_set_address(const char* text);

void pclink_log(const char* fmt, ...);  // shows up in the browser Activity panel

// Screen mirror: when pclink_wants_frame() is true, pass the TOP screen as
// row-major RGB565 (w*h pixels). 400x240 is halved to 200x120 before sending.
bool pclink_wants_frame(void);
void pclink_send_frame(const uint16_t* rgb565, int w, int h);

// Remote input: call right after you read the real buttons/sticks each frame.
void pclink_apply_input(u32* held, circlePosition* circlePad, circlePosition* cStick);

// Console commands typed in the browser arrive here (on the main thread).
void pclink_set_command_callback(pclink_cmd_cb cb);

// Sends one file to the PC (name only, no path): a header 'S' message then 16 KB 'S' chunks.
// false: not linked, the file can't be read, or the socket stalled.
bool pclink_upload_file(const char* path);

#ifdef __cplusplus
}
#endif
