// CTM stream protocol ("CTMS"): video ES + cursor metadata over one TCP
// stream. Host listens; the receiver (local preview now, webOS TV later)
// connects. All integers little-endian, structs packed.
#pragma once
#include <stdint.h>

#pragma pack(push, 1)

#define CTMS_MAGIC      0x534D5443u   // 'CTMS'
#define CTMS_PORT       48056

enum CtmsType : uint16_t {
    CTMS_STREAM_INFO  = 1,   // CtmsStreamInfo (sent on connect + every encoder reconfigure)
    CTMS_VIDEO_FRAME  = 2,   // Annex-B ES payload; hdr.pts = frame pts, flags bit0 = IDR
    CTMS_CURSOR_POS   = 3,   // CtmsCursorPos (sent on change)
    CTMS_CURSOR_SHAPE = 4,   // CtmsCursorShape + RGBA8 pixels (sent when shape changes)
};

#define CTMS_FLAG_IDR   0x0001

struct CtmsHdr {
    uint32_t magic;          // CTMS_MAGIC
    uint16_t type;           // CtmsType
    uint16_t flags;
    uint64_t pts;            // video: frame pts; cursor: sender ms timestamp
    uint32_t payloadLen;     // bytes following this header
};

struct CtmsStreamInfo {
    uint16_t codec;          // 1 = HEVC, 2 = AV1
    uint16_t width, height;
    uint16_t fps;
    uint8_t  isHDR;          // 1 = PQ BT.2020 10-bit content
    uint8_t  _pad;
    // HDR10 statics (valid when isHDR)
    float    primaries[8];   // rx,ry,gx,gy,bx,by,wx,wy
    float    maxLum, minLum; // mastering display nits
    float    maxCLL, maxFALL;
};

struct CtmsCursorPos {
    int32_t x, y;            // top-left of shape, capture-space pixels
    uint8_t visible;
    uint8_t _pad[3];
};

struct CtmsCursorShape {
    uint16_t width, height;
    int16_t  hotX, hotY;
    // followed by width*height*4 bytes RGBA8
};

#pragma pack(pop)
