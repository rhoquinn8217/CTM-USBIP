// ---------------------------------------------------------------------------
// The DS4 confirmation tones, encoded to SBC once and emitted as C.
//
// WHY THIS EXISTS. The DualSense's tone table says "GENERATED. Do not edit the
// bytes" and the tool that made it was never committed, so its settings lived
// only in a comment. T-238 needed the same thing for a DS4 and had nothing to
// start from. This is that tool, committed this time.
//
// WHY IT IS NOT A NEW ENCODER. The listener already encodes SBC for this exact
// pad, and that path is proven on hardware -- T-203 heard it. The setup below
// is copied from CtmMapRuntime::ensure_sbc_encoder VERBATIM, including the poke
// into FFmpeg's PRIVATE sbc_frame struct. That poke is not optional: FFmpeg's
// SBC encoder exposes no way to fix the frame parameters, and without it the
// frames come out a different size and the pad is handed a malformed report.
//
// WHY A SEPARATE BINARY rather than a mode on the listener: running it must not
// mean rebuilding and restarting the listener, which would mean releasing a
// bridged controller first.
//
// The parameters are the map's, not a choice made here --
// maps/ds4_usb_over_ds4_bt.map, LAYOUT B:
//   32000 Hz, joint stereo, loudness, bitpool 48, 16 blocks, 8 subbands
//   -> 109 bytes per frame, 128 samples per frame, 4 ms of audio.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/sbc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

struct FfmpegSbcPrivPrefix {
    const AVClass *avClass = nullptr;
    int64_t maxDelay = 0;
    int msbc = 0;
    alignas(SBC_ALIGN) struct sbc_frame frame;
};

static const uint32_t kRate       = 32000;
static const uint8_t  kBitpool    = 48;
static const uint8_t  kBlocks     = 16;
static const uint8_t  kSubbands   = 8;
static const size_t   kSamples    = (size_t)kBlocks * kSubbands;   /* 128 */
static const size_t   kFrameBytes = 109;

/* Matched to the DualSense's, so the two pads speak the same vocabulary.
 * 140 ms there; 144 here because a report carries two 4 ms frames and an odd
 * frame count would leave half a report to pad. */
static const int kToneFrames = 36;   /* 144 ms */
static const int kGapFrames  = 10;   /*  40 ms */

static const double kPi = 3.14159265358979323846;

static AVCodecContext *g_ctx = nullptr;
static AVFrame *g_frame = nullptr;
static AVPacket *g_packet = nullptr;

static bool open_encoder()
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_SBC);
    if (!codec) { fprintf(stderr, "no SBC encoder in this avcodec\n"); return false; }
    g_ctx = avcodec_alloc_context3(codec);
    if (!g_ctx) return false;

    g_ctx->sample_rate = (int)kRate;
    g_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    g_ctx->bit_rate = 225000;
    g_ctx->global_quality = (int)kBitpool * FF_QP2LAMBDA;
    av_channel_layout_default(&g_ctx->ch_layout, 2);

    if (avcodec_open2(g_ctx, codec, nullptr) < 0) {
        fprintf(stderr, "avcodec_open2 failed\n");
        return false;
    }

    /* VERBATIM from the listener. Do not "tidy" this. */
    FfmpegSbcPrivPrefix *priv = (FfmpegSbcPrivPrefix *)g_ctx->priv_data;
    priv->frame.frequency  = SBC_FREQ_32000;
    priv->frame.blocks     = (decltype(priv->frame.blocks))kBlocks;
    priv->frame.mode       = (decltype(priv->frame.mode))SBC_MODE_JOINT_STEREO;
    priv->frame.channels   = 2;
    priv->frame.allocation = (decltype(priv->frame.allocation))SBC_AM_LOUDNESS;
    priv->frame.subbands   = (decltype(priv->frame.subbands))kSubbands;
    priv->frame.bitpool    = (decltype(priv->frame.bitpool))kBitpool;
    priv->frame.codesize   = (uint16_t)(kSubbands * kBlocks * 2 * sizeof(int16_t));
    g_ctx->frame_size = kSubbands * kBlocks;

    g_frame = av_frame_alloc();
    g_packet = av_packet_alloc();
    return g_frame != nullptr && g_packet != nullptr;
}

/* One 128-sample stereo block in, one 109-byte SBC frame out. */
static bool encode_one(const int16_t *interleaved, std::vector<uint8_t> *out)
{
    av_frame_unref(g_frame);
    g_frame->format = AV_SAMPLE_FMT_S16;
    g_frame->sample_rate = (int)kRate;
    g_frame->nb_samples = (int)kSamples;
    av_channel_layout_default(&g_frame->ch_layout, 2);
    if (av_frame_get_buffer(g_frame, 0) < 0) return false;
    memcpy(g_frame->data[0], interleaved, kSamples * 2 * sizeof(int16_t));

    if (avcodec_send_frame(g_ctx, g_frame) < 0) return false;
    out->clear();
    for (;;) {
        int ret = avcodec_receive_packet(g_ctx, g_packet);
        if (ret == AVERROR(EAGAIN)) break;
        if (ret < 0) return false;
        out->insert(out->end(), g_packet->data, g_packet->data + g_packet->size);
        av_packet_unref(g_packet);
    }
    return true;
}

/* The DualSense's envelope, and its reasoning, which was learned by ear:
 * attack, HOLD, then a short release. A triangular envelope dwindles to
 * nothing, and on a speaker this small a low note is inaudible before it has
 * finished -- so every signal ending on a low note sounded cut off. */
static double envelope(int frame, int total)
{
    const double attack = 4.0;    /* 16 ms */
    const double release = 6.0;   /* 24 ms */
    if ((double)frame < attack) return ((double)frame + 1.0) / (attack + 1.0);
    if ((double)frame >= (double)total - release) {
        const double left = (double)(total - frame);
        return left / (release + 1.0);
    }
    return 1.0;
}

static bool render_note(double hz, double gain, std::vector<std::vector<uint8_t> > *frames)
{
    frames->clear();
    double phase = 0.0;
    const double step = 2.0 * kPi * hz / (double)kRate;
    std::vector<int16_t> pcm(kSamples * 2);
    for (int f = 0; f < kToneFrames; ++f) {
        const double env = envelope(f, kToneFrames);
        for (size_t i = 0; i < kSamples; ++i) {
            double v = sin(phase) * env * gain * 28000.0;
            phase += step;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            const int16_t s = (int16_t)v;
            /* THE SAME SAMPLE IN BOTH CHANNELS. The map's route model says a
             * plain speaker route is a SPLIT -- channel 0 to the speaker,
             * channel 1 to headphone-L -- so duplicated content is what makes
             * one tone reach either. */
            pcm[i * 2] = s;
            pcm[i * 2 + 1] = s;
        }
        std::vector<uint8_t> enc;
        if (!encode_one(&pcm[0], &enc)) {
            fprintf(stderr, "encode failed at frame %d\n", f);
            return false;
        }
        if (enc.size() != kFrameBytes) {
            fprintf(stderr, "frame %d came out %u bytes, expected %u -- "
                            "the private struct poke is wrong\n",
                    f, (unsigned)enc.size(), (unsigned)kFrameBytes);
            return false;
        }
        frames->push_back(enc);
    }
    return true;
}

static void emit_bytes(FILE *o, const std::vector<uint8_t> &b, const char *indent)
{
    fprintf(o, "%s", indent);
    for (size_t i = 0; i < b.size(); ++i) {
        fprintf(o, "0x%02x,", b[i]);
        if ((i % 12) == 11 && i + 1 < b.size()) fprintf(o, "\n%s", indent);
        else if (i + 1 < b.size()) fprintf(o, " ");
    }
    fprintf(o, "\n");
}

static void emit_table(FILE *o, const char *name, const std::vector<std::vector<uint8_t> > &f)
{
    fprintf(o, "static const uint8_t %s[DS4SIG_TONE_FRAMES][DS4SIG_FRAME_BYTES] = {\n", name);
    for (size_t i = 0; i < f.size(); ++i) {
        fprintf(o, "    {   /* %u */\n", (unsigned)i);
        emit_bytes(o, f[i], "        ");
        fprintf(o, "    },\n");
    }
    fprintf(o, "};\n\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: ds4-tone-gen <out.inl>\n"); return 2; }
    if (!open_encoder()) return 1;

    /* Silence, for the prime. Encoded rather than zeroed: an SBC frame has a
     * header and scale factors, so "silence" is a real frame, not zero bytes. */
    std::vector<int16_t> quiet(kSamples * 2, 0);
    std::vector<uint8_t> silence;
    if (!encode_one(&quiet[0], &silence) || silence.size() != kFrameBytes) {
        fprintf(stderr, "silence frame came out %u bytes\n", (unsigned)silence.size());
        return 1;
    }

    /* AND THE LOW NOTES ARE LOUDER, for the DualSense's reason: the speaker
     * rolls off at the bottom, so equal amplitude is not equal loudness. */
    std::vector<std::vector<uint8_t> > low, lower;
    if (!render_note(660.0, 0.85, &low)) return 1;
    if (!render_note(495.0, 1.00, &lower)) return 1;

    FILE *o = fopen(argv[1], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }

    fprintf(o,
        "/* --- the DS4's Bluetooth confirmation tones, pre-encoded ------------------\n"
        " *\n"
        " * GENERATED by tools/ds4-tone-gen in the listener repo, and NOT hand edited.\n"
        " * Unlike the DualSense's table next door, the tool that made this one IS\n"
        " * committed, so these bytes are reproducible rather than merely described.\n"
        " * ASCII only on purpose: a generated file is regenerated, not read.\n"
        " *\n"
        " * THE FORMAT IS THE MAP'S, NOT A CHOICE MADE HERE.\n"
        " * maps/ds4_usb_over_ds4_bt.map, LAYOUT B: 32000 Hz, joint stereo, loudness,\n"
        " * bitpool 48, 16 blocks, 8 subbands -> 109 bytes per frame, 128 samples,\n"
        " * 4 ms. A 0x14 report carries TWO of these. Those numbers are what the pad\n"
        " * is already fed by the host every time a game plays a sound, so they are\n"
        " * proven rather than derived.\n"
        " *\n"
        " * REFUSED is low then LOWER -- sinking, it did not happen. The same\n"
        " * vocabulary the DualSense uses, at the same two frequencies, so one pad\n"
        " * does not mean something different from the other.\n"
        " *\n"
        " * The envelope HOLDS rather than fades, and the low note is lifted -- both\n"
        " * learned by ear on the DualSense and carried over rather than\n"
        " * rediscovered. See ctm_bt_signal_data.inl. */\n\n");

    fprintf(o, "#define DS4SIG_FRAME_BYTES  %u\n", (unsigned)kFrameBytes);
    fprintf(o, "#define DS4SIG_TONE_FRAMES  %d   /* 144 ms per note */\n", kToneFrames);
    fprintf(o, "#define DS4SIG_GAP_FRAMES   %d   /* 40 ms, so two notes read as two */\n\n",
            kGapFrames);

    fprintf(o, "static const uint8_t g_ds4sig_silence[DS4SIG_FRAME_BYTES] = {\n");
    emit_bytes(o, silence, "    ");
    fprintf(o, "};\n\n");

    fprintf(o, "/* 660 Hz */\n");
    emit_table(o, "g_ds4sig_low", low);
    fprintf(o, "/* 495 Hz */\n");
    emit_table(o, "g_ds4sig_lower", lower);
    fclose(o);

    printf("wrote %s: silence + %d low + %d lower, %u bytes each\n",
           argv[1], kToneFrames, kToneFrames, (unsigned)kFrameBytes);
    return 0;
}
