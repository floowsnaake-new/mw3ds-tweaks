// Helper: grab top screen as row-major RGB565 for pclink_send_frame
#include "pclink.h"
#include "pclink_frame.h"
#include <3ds.h>

void pclink_grab_and_send_frame(void)
{
    if (!pclink_wants_frame())
        return;

    const u8* fb = (const u8*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) return;

    GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(GFX_TOP);
    static uint16_t buf[400 * 240];

    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 400; x++) {
            int idx = x * 240 + (239 - y);
            uint16_t p = 0;
            switch (fmt) {
            case GSP_RGB565_OES:
                p = ((const u16*)fb)[idx];
                break;
            case GSP_RGBA8_OES: {
                const u8* px = fb + idx * 4;
                u8 r = px[0], g = px[1], b = px[2];
                p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                break;
            }
            case GSP_BGR8_OES: {
                const u8* px = fb + idx * 3;
                u8 b = px[0], g = px[1], r = px[2];
                p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                break;
            }
            default:
                p = 0;
                break;
            }
            buf[y * 400 + x] = p;
        }
    }
    pclink_send_frame(buf, 400, 240);
}
