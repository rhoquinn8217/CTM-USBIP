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
    CTMS_VIDEO_FRAME  = 2,   // Annex-B ES payload; hdr.pts=t0 present, hdr.t1=encoded; flags bit0=IDR
    CTMS_CURSOR_POS   = 3,   // CtmsCursorPos (sent on change)
    CTMS_CURSOR_SHAPE = 4,   // CtmsCursorShape + RGBA8 pixels (sent when shape changes)
    CTMS_PING         = 5,   // client->host: CtmsPing (clock sync)
    CTMS_PONG         = 6,   // host->client: CtmsPong (echo + host clock)
    CTMS_AUDIO_FRAME  = 7,   // interleaved S16LE PCM; hdr.pts = capture time (us)
};

#define CTMS_FLAG_IDR   0x0001

// All host timestamps are microseconds since the host's stream epoch (t0_ at
// stream start). pts/t1/tSend let the receiver build a present->encode->send->
// arrive timeline; PING/PONG resolve the host<->client clock offset + RTT, so
// the client can turn tSend into real one-way transport ((arrive + offset) - tSend).
struct CtmsHdr {
    uint32_t magic;          // CTMS_MAGIC
    uint16_t type;           // CtmsType
    uint16_t flags;
    uint64_t pts;            // video: t0 = Windows present time (us); cursor: send time
    uint64_t tEnc;           // video: encode-START time (us); enc = t1 - tEnc
    uint64_t t1;             // video: encode-done time (us); else 0
    uint64_t tSend;          // video: send-departure time (us): frame's first byte to socket
    uint32_t payloadLen;     // bytes following this header
};

struct CtmsPing {
    uint64_t clientUs;       // client clock at send (echoed back)
};
struct CtmsPong {
    uint64_t clientUs;       // echo of CtmsPing.clientUs
    uint64_t hostUs;         // host clock (us since host stream epoch) at reply
    uint64_t hostWallUs;     // host REAL wall clock (us since Unix epoch) at reply
};

// UDP clock-sync side-channel (host binds UDP CTMS_PORT). Off the TCP stream so the
// exchange isn't stuck behind video frames -> prompt stamps both ends. Full 4-timestamp
// NTP in REAL wall-clock us: offset(PC-TV) = ((t1-t0)+(t2-t3))/2, rtt = (t3-t0)-(t2-t1).
#define CTMS_CLOCK_MAGIC 0x434C4F43u // 'CLOC'
struct CtmsClockPing {
    uint32_t magic;   // CTMS_CLOCK_MAGIC
    uint64_t t0;      // TV wall us at PING send
};
struct CtmsClockPong {
    uint32_t magic;   // CTMS_CLOCK_MAGIC
    uint64_t t0;      // echoed TV send
    uint64_t t1;      // host wall us at PING receive
    uint64_t t2;      // host wall us at PONG send
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
    uint8_t  hasAudio;       // 1 = an audio stream follows (S16LE PCM)
    uint8_t  audioChannels;  // e.g. 2
    uint16_t _apad;
    uint32_t audioRate;      // PCM sample rate, Hz (e.g. 48000)
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
