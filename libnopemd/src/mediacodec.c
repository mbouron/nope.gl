/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/hardware_buffer.h>
#include <libavcodec/bsf.h>
#include <libavutil/attributes.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include "mediacodec.h"

#define MAX_PTS_ENTRIES 64

/* Maximum wait for a rendered frame to reach the image reader */
#define RENDER_TIMEOUT_US 250000

#define RELEASE_WARNING_US 5000000

/* Resets without returning a frame before switching to software or giving up */
#define MAX_RESETS 3

/* Bound memory use when key frames are far apart */
#define MAX_REPLAY_PKTS 1024
#define MAX_REPLAY_SIZE (32 << 20)

/* Spread packet freeing across sends to avoid a pause after a large GOP */
#define FREE_PKTS_PER_SEND 2

/* AudioFormat encodings (MediaFormat.KEY_PCM_ENCODING) */
#define PCM_ENCODING_16BIT 2
#define PCM_ENCODING_8BIT  3
#define PCM_ENCODING_FLOAT 4

#define H264_NAL_SPS 7
#define H264_NAL_PPS 8

static const struct mc_desc {
    enum AVCodecID codec_id;
    const char *mime;
    const char *sw_names[2]; // in order of preference
} descs[] = {
    {AV_CODEC_ID_H264,   "video/avc",           {"c2.android.avc.decoder"}},
    {AV_CODEC_ID_HEVC,   "video/hevc",          {"c2.android.hevc.decoder"}},
    {AV_CODEC_ID_MPEG4,  "video/mp4v-es",       {"c2.android.mpeg4.decoder"}},
    {AV_CODEC_ID_VP8,    "video/x-vnd.on2.vp8", {"c2.android.vp8.decoder"}},
    {AV_CODEC_ID_VP9,    "video/x-vnd.on2.vp9", {"c2.android.vp9.decoder"}},
    {AV_CODEC_ID_AV1,    "video/av01",          {"c2.android.av1-dav1d.decoder", "c2.android.av1.decoder"}},
    {AV_CODEC_ID_AAC,    "audio/mp4a-latm",     {"c2.android.aac.decoder"}},
    {AV_CODEC_ID_AMR_NB, "audio/3gpp",          {"c2.android.amrnb.decoder"}},
    {AV_CODEC_ID_AMR_WB, "audio/amr-wb",        {"c2.android.amrwb.decoder"}},
};

static const struct mc_desc *get_desc(enum AVCodecID codec_id)
{
    for (size_t i = 0; i < FF_ARRAY_ELEMS(descs); i++)
        if (descs[i].codec_id == codec_id)
            return &descs[i];

    return NULL;
}

/* OMX names are aliases of the platform Codec2 decoders */
static bool is_platform_software(const char *name)
{
    return !strncmp(name, "c2.android.", strlen("c2.android.")) ||
           !strncmp(name, "OMX.google.", strlen("OMX.google."));
}

static const char *get_status_str(media_status_t status)
{
    switch (status) {
    case AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE: return "insufficient resources";
    case AMEDIACODEC_ERROR_RECLAIMED:             return "reclaimed";
    case AMEDIA_ERROR_MALFORMED:                  return "malformed";
    case AMEDIA_ERROR_UNSUPPORTED:                return "unsupported";
    case AMEDIA_ERROR_INVALID_OBJECT:             return "invalid object";
    case AMEDIA_ERROR_INVALID_PARAMETER:          return "invalid parameter";
    case AMEDIA_ERROR_INVALID_OPERATION:          return "invalid operation";
    case AMEDIA_ERROR_IO:                         return "I/O error";
    default:                                      return "error";
    }
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000;
}

/* Frames keep the image reader alive so they remain valid after decoder destruction */
struct mc_reader {
    atomic_int refcount;
    AImageReader *reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

struct mc_image {
    struct mc_reader *reader;
    AImage *image;
    int release_fence_fd;
};

/* Keep original timestamps to avoid rounding when converting back from microseconds */
struct pts_entry {
    int64_t us;
    int64_t pts;
    int64_t duration;
    uint32_t skip_samples;    // audio samples to skip at the start (AV_PKT_DATA_SKIP_SAMPLES)
    uint32_t discard_padding; // audio samples to discard at the end
    bool used;
};

struct nmdi_mediacodec {
    void *log_ctx;
    void (*log)(void *log_ctx, int level, const char *fmt, va_list vl);

    const AVCodecParameters *par;
    enum AVMediaType type;
    AVRational time_base;
    const struct mc_desc *desc;
    bool software;
    char *codec_name;

    AMediaCodec *codec;
    struct mc_reader *reader;
    ANativeWindow *window;
    int64_t render_seq;

    AVBSFContext *bsf;
    AVPacket *pkt;

    ssize_t input_index;     // input buffer dequeued ahead, -1 if none
    bool draining;           // end of stream requested, kept across resets
    bool eos_pending;
    bool eos;
    bool need_key_frame;
    int nb_resets;           // consecutive resets without returning a frame

    /* Keep packets from the last key frame so a reset can resume decoding */
    AVPacket **replay_pkts;
    size_t nb_replay_pkts;
    size_t replay_size;
    bool replay_valid;       // every packet since the last key frame is held
    size_t replay_pos;       // next packet to replay, nb_replay_pkts when done

    AVPacket **old_pkts;     // freed gradually as new packets arrive
    size_t nb_old_pkts;

    int64_t last_output_pts; // AV_NOPTS_VALUE if none
    bool skip_output;        // discard frames already output before a reset

    int width;
    int height;
    enum AVColorRange color_range;
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;

    int sample_rate;
    int nb_channels;
    enum AVSampleFormat sample_fmt;
    int64_t skip_samples;    // remaining samples to skip across output frames

    struct pts_entry pts_entries[MAX_PTS_ENTRIES];
    size_t pts_entries_pos;
};

static av_printf_format(3, 4) void log_msg(struct nmdi_mediacodec *s, int level, const char *fmt, ...)
{
    if (!s->log)
        return;

    va_list vl;
    va_start(vl, fmt);
    s->log(s->log_ctx, level, fmt, vl);
    va_end(vl);
}

#define LOG(s, level, ...) log_msg(s, AV_LOG_##level, __VA_ARGS__)

static struct mc_reader *reader_ref(struct mc_reader *reader)
{
    atomic_fetch_add(&reader->refcount, 1);
    return reader;
}

static void reader_unrefp(struct mc_reader **readerp)
{
    struct mc_reader *reader = *readerp;
    if (!reader)
        return;

    if (atomic_fetch_sub(&reader->refcount, 1) == 1) {
        if (reader->reader)
            AImageReader_delete(reader->reader);
        pthread_cond_destroy(&reader->cond);
        pthread_mutex_destroy(&reader->lock);
        av_freep(readerp);
    }

    *readerp = NULL;
}

static void signal_reader(void *context, AImageReader *image_reader)
{
    struct mc_reader *reader = context;

    pthread_mutex_lock(&reader->lock);
    pthread_cond_broadcast(&reader->cond);
    pthread_mutex_unlock(&reader->lock);
}

static int wait_reader(struct mc_reader *reader, int64_t deadline_us)
{
    const struct timespec ts = {
        .tv_sec  = deadline_us / 1000000,
        .tv_nsec = (deadline_us % 1000000) * 1000,
    };
    return pthread_cond_timedwait(&reader->cond, &reader->lock, &ts);
}

static int init_reader(struct nmdi_mediacodec *s, int max_images)
{
    struct mc_reader *reader = av_mallocz(sizeof(*reader));
    if (!reader)
        return AVERROR(ENOMEM);

    atomic_init(&reader->refcount, 1);
    if (pthread_mutex_init(&reader->lock, NULL)) {
        av_free(reader);
        return AVERROR_EXTERNAL;
    }

    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        pthread_mutex_destroy(&reader->lock);
        av_free(reader);
        return AVERROR_EXTERNAL;
    }

    int err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!err)
        err = pthread_cond_init(&reader->cond, &attr);
    pthread_condattr_destroy(&attr);
    if (err) {
        pthread_mutex_destroy(&reader->lock);
        av_free(reader);
        return AVERROR_EXTERNAL;
    }

    s->reader = reader;

    media_status_t status = AImageReader_newWithUsage(s->par->width, s->par->height, AIMAGE_FORMAT_PRIVATE,
                                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, max_images,
                                                      &reader->reader);
    if (status != AMEDIA_OK) {
        LOG(s, ERROR, "unable to create an image reader of %d images: %s (%d)", max_images,
            get_status_str(status), status);
        return AVERROR_EXTERNAL;
    }

    AImageReader_ImageListener listener = {
        .context = reader,
        .onImageAvailable = signal_reader,
    };
    status = AImageReader_setImageListener(reader->reader, &listener);
    if (status != AMEDIA_OK)
        return AVERROR_EXTERNAL;

    status = AImageReader_getWindow(reader->reader, &s->window);
    if (status != AMEDIA_OK)
        return AVERROR_EXTERNAL;

    return 0;
}

static void free_image(void *opaque, uint8_t *data)
{
    struct mc_image *img = (struct mc_image *)data;
    struct mc_reader *reader = img->reader;

    if (img->release_fence_fd >= 0)
        AImage_deleteAsync(img->image, img->release_fence_fd);
    else
        AImage_delete(img->image);

    signal_reader(reader, NULL);
    reader_unrefp(&img->reader);
    av_free(img);
}

static void wait_fence(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ret;
    do {
        ret = poll(&pfd, 1, -1);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "could not wait for the release fence: %s\n", strerror(errno));
        return;
    }

    if (!(pfd.revents & POLLIN) || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        av_log(NULL, AV_LOG_ERROR, "unexpected release fence events: %#x\n", pfd.revents);
}

void nmdi_mediacodec_release_frame(AVFrame **framep, int release_fence_fd)
{
    AVFrame *frame = *framep;
    if (release_fence_fd >= 0) {
        if (frame && frame->format == AV_PIX_FMT_MEDIACODEC) {
            struct mc_image *img = (struct mc_image *)frame->buf[0]->data;
            pthread_mutex_lock(&img->reader->lock);
            const int prev_fence_fd = img->release_fence_fd;
            img->release_fence_fd = release_fence_fd;
            pthread_mutex_unlock(&img->reader->lock);

            /*
             * Multiple references can use the same image on the GPU. Keep the
             * new fence for image release and wait for the previous use here.
             */
            if (prev_fence_fd >= 0) {
                wait_fence(prev_fence_fd);
                close(prev_fence_fd);
            }
        } else {
            close(release_fence_fd);
        }
    }

    av_frame_free(framep);
}

/*
 * Match the image to the render sequence number. Images from earlier renders
 * may still arrive after a timeout; discard them so they cannot replace this frame.
 */
static int acquire_image(struct nmdi_mediacodec *s, int64_t seq, AImage **imagep)
{
    struct mc_reader *reader = s->reader;
    int64_t render_deadline = now_us() + RENDER_TIMEOUT_US;
    int64_t warning_time = AV_NOPTS_VALUE;
    int ret = 0;

    pthread_mutex_lock(&reader->lock);
    for (;;) {
        AImage *image = NULL;
        const media_status_t status = AImageReader_acquireNextImage(reader->reader, &image);
        if (status == AMEDIA_OK) {
            int64_t timestamp = 0;
            AImage_getTimestamp(image, &timestamp);
            if (timestamp == seq) {
                *imagep = image;
                break;
            }

            AImage_delete(image);
            continue;
        }

        const int64_t t = now_us();
        if (status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
            /*
             * The reader cannot acquire more images until previously returned frames
             * are released. Exclude this wait from the rendering timeout.
             */
            if (warning_time == AV_NOPTS_VALUE) {
                warning_time = t + RELEASE_WARNING_US;
            } else if (t >= warning_time) {
                LOG(s, WARNING, "every image is held, waiting for frames to be released");
                warning_time = t + RELEASE_WARNING_US;
            }

            wait_reader(reader, warning_time);
            render_deadline = now_us() + RENDER_TIMEOUT_US;
            continue;
        }

        if (status != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            LOG(s, ERROR, "unable to acquire an image: %s (%d)", get_status_str(status), status);
            ret = AVERROR_EXTERNAL;
            break;
        }

        if (t >= render_deadline) {
            ret = AVERROR(ETIMEDOUT);
            break;
        }

        wait_reader(reader, render_deadline);
    }
    pthread_mutex_unlock(&reader->lock);

    return ret;
}

static bool is_start_code(const uint8_t *data, size_t pos, size_t size)
{
    return pos + 3 <= size && !data[pos] && !data[pos + 1] && data[pos + 2] == 1;
}

/* Read an Annex B NAL unit without its start code or trailing zeros */
static bool next_nal_unit(const uint8_t *data, size_t size, size_t *posp, const uint8_t **nalp, size_t *nal_sizep)
{
    size_t pos = *posp;
    while (pos < size && !is_start_code(data, pos, size))
        pos++;
    if (pos == size)
        return false;

    const size_t nal_start = pos + 3;
    size_t nal_end = nal_start;
    while (nal_end < size && !is_start_code(data, nal_end, size))
        nal_end++;
    *posp = nal_end;

    /* A four-byte start code leaves its first zero at the end of this NAL unit */
    while (nal_end > nal_start && !data[nal_end - 1])
        nal_end--;

    *nalp = data + nal_start;
    *nal_sizep = nal_end - nal_start;
    return true;
}

/* MediaCodec expects SPS in csd-0 and PPS in csd-1 */
static int set_h264_parameter_sets(AMediaFormat *format, const uint8_t *data, size_t size)
{
    uint8_t *csd[2] = {NULL, NULL};
    size_t csd_size[2] = {0, 0};
    int ret = 0;

    const uint8_t *nal;
    size_t nal_size, pos = 0;
    while (next_nal_unit(data, size, &pos, &nal, &nal_size)) {
        const int type = nal_size ? nal[0] & 0x1f : 0;
        const int idx = type == H264_NAL_SPS ? 0 : type == H264_NAL_PPS ? 1 : -1;
        if (idx < 0)
            continue;

        uint8_t *buf = av_realloc(csd[idx], csd_size[idx] + 4 + nal_size);
        if (!buf) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        memcpy(buf + csd_size[idx], (const uint8_t[]){0, 0, 0, 1}, 4);
        memcpy(buf + csd_size[idx] + 4, nal, nal_size);
        csd[idx] = buf;
        csd_size[idx] += 4 + nal_size;
    }

    if (csd[0])
        AMediaFormat_setBuffer(format, "csd-0", csd[0], csd_size[0]);
    if (csd[1])
        AMediaFormat_setBuffer(format, "csd-1", csd[1], csd_size[1]);

end:
    av_freep(&csd[0]);
    av_freep(&csd[1]);
    return ret;
}

static int set_codec_specific_data(struct nmdi_mediacodec *s, AMediaFormat *format)
{
    const AVCodecParameters *par = s->bsf ? s->bsf->par_out : s->par;
    const uint8_t *data = par->extradata;
    const size_t size = (size_t)par->extradata_size;

    switch (par->codec_id) {
    case AV_CODEC_ID_H264:
        return size ? set_h264_parameter_sets(format, data, size) : 0;
    case AV_CODEC_ID_HEVC:  // VPS, SPS and PPS
    case AV_CODEC_ID_MPEG4: // VOL header
    case AV_CODEC_ID_AV1:   // av1C
        if (size)
            AMediaFormat_setBuffer(format, "csd-0", data, size);
        return 0;
    case AV_CODEC_ID_AAC:   // AudioSpecificConfig, ADTS headers in the packets otherwise
        if (size)
            AMediaFormat_setBuffer(format, "csd-0", data, size);
        else
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_IS_ADTS, 1);
        return 0;
    default:
        return 0;
    }
}

/* MediaCodec expects Annex B; convert length-prefixed H.264/HEVC packets before sending them */
static int init_bsf(struct nmdi_mediacodec *s)
{
    const AVCodecParameters *par = s->par;
    const char *name = par->codec_id == AV_CODEC_ID_H264 ? "h264_mp4toannexb"
                     : par->codec_id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb"
                     : NULL;
    if (!name || par->extradata_size < 1 || par->extradata[0] != 1)
        return 0;

    const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
    if (!filter) {
        LOG(s, ERROR, "%s bitstream filter not available", name);
        return AVERROR_BSF_NOT_FOUND;
    }

    int ret = av_bsf_alloc(filter, &s->bsf);
    if (ret < 0)
        return ret;

    ret = avcodec_parameters_copy(s->bsf->par_in, par);
    if (ret < 0)
        return ret;

    s->bsf->time_base_in = s->time_base;
    return av_bsf_init(s->bsf);
}

static void reset_format(struct nmdi_mediacodec *s)
{
    const AVCodecParameters *par = s->par;

    s->width = par->width;
    s->height = par->height;
    s->color_range = par->color_range;
    s->color_primaries = par->color_primaries;
    s->color_trc = par->color_trc;
    s->colorspace = par->color_space;

    s->sample_rate = par->sample_rate;
    s->nb_channels = par->ch_layout.nb_channels;
    s->sample_fmt = AV_SAMPLE_FMT_S16;
}

/* MediaCodec color constants (MediaFormat.COLOR_*) */
static void update_color(struct nmdi_mediacodec *s, AMediaFormat *format)
{
    int32_t range, standard, transfer;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, &range)) {
        switch (range) {
        case 1: s->color_range = AVCOL_RANGE_JPEG; break;
        case 2: s->color_range = AVCOL_RANGE_MPEG; break;
        }
    }

    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, &standard)) {
        switch (standard) {
        case 1: s->colorspace = AVCOL_SPC_BT709;      s->color_primaries = AVCOL_PRI_BT709;     break;
        case 2: s->colorspace = AVCOL_SPC_BT470BG;    s->color_primaries = AVCOL_PRI_BT470BG;   break;
        case 4: s->colorspace = AVCOL_SPC_SMPTE170M;  s->color_primaries = AVCOL_PRI_SMPTE170M; break;
        case 6: s->colorspace = AVCOL_SPC_BT2020_NCL; s->color_primaries = AVCOL_PRI_BT2020;    break;
        }
    }

    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, &transfer)) {
        switch (transfer) {
        case 1: s->color_trc = AVCOL_TRC_LINEAR;       break;
        case 3: s->color_trc = AVCOL_TRC_SMPTE170M;    break;
        case 6: s->color_trc = AVCOL_TRC_SMPTE2084;    break;
        case 7: s->color_trc = AVCOL_TRC_ARIB_STD_B67; break;
        }
    }
}

static void update_audio_format(struct nmdi_mediacodec *s, AMediaFormat *format)
{
    int32_t sample_rate, nb_channels, encoding;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sample_rate) && sample_rate > 0)
        s->sample_rate = sample_rate;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &nb_channels) && nb_channels > 0)
        s->nb_channels = nb_channels;

    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, &encoding)) {
        switch (encoding) {
        case PCM_ENCODING_16BIT: s->sample_fmt = AV_SAMPLE_FMT_S16; break;
        case PCM_ENCODING_8BIT:  s->sample_fmt = AV_SAMPLE_FMT_U8;  break;
        case PCM_ENCODING_FLOAT: s->sample_fmt = AV_SAMPLE_FMT_FLT; break;
        default:
            LOG(s, ERROR, "unsupported PCM encoding %d", (int)encoding);
            s->sample_fmt = AV_SAMPLE_FMT_NONE;
            break;
        }
    }
}

static void update_format(struct nmdi_mediacodec *s)
{
    AMediaFormat *format = AMediaCodec_getOutputFormat(s->codec);
    if (!format)
        return;

    LOG(s, DEBUG, "output format: %s", AMediaFormat_toString(format));

    if (s->type == AVMEDIA_TYPE_AUDIO) {
        update_audio_format(s, format);
        AMediaFormat_delete(format);
        return;
    }

    /* The right and bottom edges are inclusive. Older devices only expose crop-* keys. */
    int32_t left, top, right, bottom;
    const bool has_crop = AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom) ||
                          (AMediaFormat_getInt32(format, "crop-left", &left) &&
                          AMediaFormat_getInt32(format, "crop-top", &top) &&
                          AMediaFormat_getInt32(format, "crop-right", &right) &&
                          AMediaFormat_getInt32(format, "crop-bottom", &bottom));
    if (has_crop && right >= left && bottom >= top) {
        s->width = right - left + 1;
        s->height = bottom - top + 1;
    }

    update_color(s, format);
    AMediaFormat_delete(format);
}

static int configure_codec(struct nmdi_mediacodec *s)
{
    const AVCodecParameters *par = s->par;

    AMediaFormat *format = AMediaFormat_new();
    if (!format)
        return AVERROR(ENOMEM);

    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, s->desc->mime);
    if (s->type == AVMEDIA_TYPE_VIDEO) {
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, par->width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, par->height);
    } else {
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, par->sample_rate);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, par->ch_layout.nb_channels);
    }

    int ret = set_codec_specific_data(s, format);
    if (ret < 0) {
        AMediaFormat_delete(format);
        return ret;
    }

    media_status_t status = AMediaCodec_configure(s->codec, format, s->window, NULL, 0);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) {
        LOG(s, WARNING, "unable to configure the %s decoder: %s (%d)", s->codec_name, get_status_str(status), status);
        return AVERROR_DECODER_NOT_FOUND;
    }

    status = AMediaCodec_start(s->codec);
    if (status != AMEDIA_OK) {
        LOG(s, WARNING, "unable to start the %s decoder: %s (%d)", s->codec_name, get_status_str(status), status);
        return AVERROR_DECODER_NOT_FOUND;
    }

    return 0;
}

static void delete_codec(struct nmdi_mediacodec *s)
{
    if (!s->codec)
        return;

    AMediaCodec_stop(s->codec);
    AMediaCodec_delete(s->codec);
    s->codec = NULL;
    s->input_index = -1;
}

static int create_codec(struct nmdi_mediacodec *s, const char *name)
{
    av_assert0(!s->codec);

    AMediaCodec *codec = name ? AMediaCodec_createCodecByName(name)
                              : AMediaCodec_createDecoderByType(s->desc->mime);
    if (!codec) {
        LOG(s, WARNING, "unable to create the MediaCodec decoder %s", name ? name : s->desc->mime);
        return AVERROR_DECODER_NOT_FOUND;
    }

    char *codec_name = NULL;
    if (AMediaCodec_getName(codec, &codec_name) != AMEDIA_OK) {
        AMediaCodec_delete(codec);
        return AVERROR_EXTERNAL;
    }

    char *codec_name_dup = av_strdup(codec_name);
    AMediaCodec_releaseName(codec, codec_name);
    if (!codec_name_dup) {
        AMediaCodec_delete(codec);
        return AVERROR(ENOMEM);
    }

    if (!name && is_platform_software(codec_name_dup)) {
        LOG(s, VERBOSE, "no hardware decoder for %s, %s is the default one", s->desc->mime, codec_name_dup);
        av_free(codec_name_dup);
        AMediaCodec_delete(codec);
        return AVERROR_DECODER_NOT_FOUND;
    }

    s->codec = codec;
    av_free(s->codec_name);
    s->codec_name = codec_name_dup;

    int ret = configure_codec(s);
    if (ret < 0) {
        delete_codec(s);
        return ret;
    }

    LOG(s, DEBUG, "%s MediaCodec decoder opened", s->codec_name);
    return 0;
}

static int open_codec(struct nmdi_mediacodec *s)
{
    if (!s->software)
        return create_codec(s, NULL);

    for (size_t i = 0; i < FF_ARRAY_ELEMS(s->desc->sw_names) && s->desc->sw_names[i]; i++) {
        const int ret = create_codec(s, s->desc->sw_names[i]);
        if (ret != AVERROR_DECODER_NOT_FOUND)
            return ret;
    }

    return AVERROR_DECODER_NOT_FOUND;
}

static void free_old_pkts(struct nmdi_mediacodec *s, size_t max)
{
    for (; max && s->nb_old_pkts; max--)
        av_packet_free(&s->old_pkts[--s->nb_old_pkts]);
}

static void clear_replay(struct nmdi_mediacodec *s)
{
    if (s->nb_replay_pkts) {
        AVPacket **pkts = av_realloc_array(s->old_pkts, s->nb_old_pkts + s->nb_replay_pkts, sizeof(*pkts));
        if (pkts) {
            memcpy(pkts + s->nb_old_pkts, s->replay_pkts, s->nb_replay_pkts * sizeof(*pkts));
            s->old_pkts = pkts;
            s->nb_old_pkts += s->nb_replay_pkts;
        } else {
            for (size_t i = 0; i < s->nb_replay_pkts; i++)
                av_packet_free(&s->replay_pkts[i]);
        }
    }

    s->nb_replay_pkts = 0;
    s->replay_size = 0;
    s->replay_valid = false;
    s->replay_pos = 0;
}

static void record_packet(struct nmdi_mediacodec *s, const AVPacket *pkt)
{
    if (pkt->flags & AV_PKT_FLAG_KEY) {
        clear_replay(s);
        s->replay_valid = true;
    }
    if (!s->replay_valid)
        return;

    /* If the whole GOP cannot be kept, recovery must wait for the next key frame */
    if (s->nb_replay_pkts == MAX_REPLAY_PKTS || s->replay_size + (size_t)pkt->size > MAX_REPLAY_SIZE) {
        clear_replay(s);
        return;
    }

    AVPacket **pkts = av_realloc_array(s->replay_pkts, s->nb_replay_pkts + 1, sizeof(*pkts));
    AVPacket *ref = pkts ? av_packet_clone(pkt) : NULL;
    if (pkts)
        s->replay_pkts = pkts;
    if (!ref) {
        clear_replay(s);
        return;
    }

    s->replay_pkts[s->nb_replay_pkts++] = ref;
    s->replay_size += (size_t)pkt->size;
    s->replay_pos = s->nb_replay_pkts;
}

/* Restart from a key frame to restore the codec's reference frames after a reset */
static void prepare_resume(struct nmdi_mediacodec *s)
{
    if (s->replay_valid && s->nb_replay_pkts) {
        s->replay_pos = 0;
        s->skip_output = s->last_output_pts != AV_NOPTS_VALUE;
        s->need_key_frame = false;
    } else {
        clear_replay(s);
        s->need_key_frame = true;
    }
}

static void reset_state(struct nmdi_mediacodec *s)
{
    if (s->bsf)
        av_bsf_flush(s->bsf);

    s->input_index = -1;
    s->eos_pending = s->draining;
    s->eos = false;
    s->skip_samples = 0;

    memset(s->pts_entries, 0, sizeof(s->pts_entries));
    s->pts_entries_pos = 0;
}

int nmdi_mediacodec_reset(struct nmdi_mediacodec *s, const char *reason)
{
    if (s->nb_resets >= MAX_RESETS) {
        if (s->software) {
            LOG(s, ERROR, "%s, giving up after %d resets of the %s decoder", reason, MAX_RESETS, s->codec_name);
            delete_codec(s);
            return AVERROR_EXTERNAL;
        }

        LOG(s, WARNING, "%s after %d resets of the %s decoder, falling back on the platform software one",
            reason, MAX_RESETS, s->codec_name);
        delete_codec(s);
        reset_state(s);
        prepare_resume(s);
        s->software = true;
        s->nb_resets = 0;

        const int ret = open_codec(s);
        if (ret < 0)
            LOG(s, ERROR, "unable to create the platform software decoder");
        return ret;
    }

    s->nb_resets++;
    LOG(s, WARNING, "%s, resetting the %s decoder", reason, s->codec_name);
    delete_codec(s);
    reset_state(s);
    prepare_resume(s);

    char *name = s->codec_name;
    s->codec_name = NULL;
    int ret = create_codec(s, name);
    if (ret < 0 && !s->software) {
        LOG(s, WARNING, "unable to create the %s decoder again, falling back on the platform software one", name);
        s->software = true;
        ret = open_codec(s);
    }

    if (ret < 0) {
        LOG(s, ERROR, "unable to reset the %s decoder", name);
        av_free(s->codec_name);
        s->codec_name = name;
        return ret;
    }

    av_free(name);
    return 0;
}

static int init_decoder(struct nmdi_mediacodec *s, const struct nmdi_mediacodec_params *params)
{
    const AVCodecParameters *par = params->par;

    s->log_ctx = params->log_ctx;
    s->log = params->log;

    s->par = par;
    s->type = par->codec_type;
    s->time_base = params->time_base;
    s->desc = get_desc(par->codec_id);
    s->software = params->software;
    s->input_index = -1;
    s->last_output_pts = AV_NOPTS_VALUE;
    reset_format(s);

    s->pkt = av_packet_alloc();
    if (!s->pkt)
        return AVERROR(ENOMEM);

    int ret = init_bsf(s);
    if (ret < 0)
        return ret;

    if (s->type == AVMEDIA_TYPE_VIDEO) {
        ret = init_reader(s, params->max_images);
        if (ret < 0)
            return ret;
    }

    return open_codec(s);
}

int nmdi_mediacodec_create(struct nmdi_mediacodec **sp, const struct nmdi_mediacodec_params *params)
{
    *sp = NULL;

    const AVCodecParameters *par = params->par;
    if (!get_desc(par->codec_id))
        return AVERROR_DECODER_NOT_FOUND;
    if (par->codec_type == AVMEDIA_TYPE_VIDEO && (par->width <= 0 || par->height <= 0 || params->max_images <= 0))
        return AVERROR_DECODER_NOT_FOUND;
    if (par->codec_type == AVMEDIA_TYPE_AUDIO && (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0))
        return AVERROR_DECODER_NOT_FOUND;

    struct nmdi_mediacodec *s = av_mallocz(sizeof(*s));
    if (!s)
        return AVERROR(ENOMEM);

    int ret = init_decoder(s, params);
    if (ret < 0) {
        nmdi_mediacodec_freep(&s);
        return ret;
    }

    *sp = s;
    return 0;
}

/* Return 1 if an input buffer is available, 0 if not, or an error */
static int dequeue_input(struct nmdi_mediacodec *s)
{
    if (s->input_index >= 0)
        return 1;

    const ssize_t index = AMediaCodec_dequeueInputBuffer(s->codec, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        return 0;
    if (index < 0) {
        LOG(s, ERROR, "unable to dequeue an input buffer: %s (%zd)", get_status_str((media_status_t)index), index);
        return AVERROR_EXTERNAL;
    }

    s->input_index = index;
    return 1;
}

static int queue_input(struct nmdi_mediacodec *s, const AVPacket *pkt, const uint8_t *data, size_t size,
                       uint32_t flags)
{
    size_t buf_size;
    uint8_t *buf = AMediaCodec_getInputBuffer(s->codec, (size_t)s->input_index, &buf_size);
    if (!buf) {
        LOG(s, ERROR, "unable to get the input buffer %zd", s->input_index);
        return AVERROR_EXTERNAL;
    }

    if (size > buf_size) {
        /* Keep the input buffer for the next packet */
        LOG(s, ERROR, "packet of %zu bytes larger than the input buffer (%zu bytes), "
            "dropping the packets until the next key frame", size, buf_size);
        clear_replay(s);
        s->need_key_frame = true;
        return 0;
    }

    if (size)
        memcpy(buf, data, size);

    const int64_t pts = pkt ? pkt->pts : AV_NOPTS_VALUE;
    const int64_t us = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, s->time_base, AV_TIME_BASE_Q) : 0;
    media_status_t status = AMediaCodec_queueInputBuffer(s->codec, (size_t)s->input_index, 0, size,
                                                         (uint64_t)us, flags);
    s->input_index = -1;
    if (status != AMEDIA_OK) {
        LOG(s, ERROR, "unable to queue an input buffer: %s (%d)", get_status_str(status), status);
        return AVERROR_EXTERNAL;
    }

    if (pkt) {
        struct pts_entry entry = {.us = us, .pts = pts, .duration = pkt->duration, .used = true};
        size_t side_size;
        const uint8_t *side = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
        if (side && side_size >= 10) {
            entry.skip_samples = AV_RL32(side);
            entry.discard_padding = AV_RL32(side + 4);
        }

        s->pts_entries[s->pts_entries_pos] = entry;
        s->pts_entries_pos = (s->pts_entries_pos + 1) % MAX_PTS_ENTRIES;
    }

    return 0;
}

/* Queue a packet: 1 once consumed, 0 if no input buffer is available */
static int queue_packet(struct nmdi_mediacodec *s, const AVPacket *pkt)
{
    int ret = dequeue_input(s);
    if (ret <= 0)
        return ret;

    ret = av_packet_ref(s->pkt, pkt);
    if (ret < 0)
        return ret;

    if (s->bsf) {
        ret = av_bsf_send_packet(s->bsf, s->pkt);
        if (ret >= 0)
            ret = av_bsf_receive_packet(s->bsf, s->pkt);
        if (ret < 0) {
            av_packet_unref(s->pkt);
            /* Skip corrupt packets and keep the input buffer for the next one */
            LOG(s, WARNING, "unable to filter packet (pts=%" PRId64 "): %s", pkt->pts, av_err2str(ret));
            return 1;
        }
    }

    ret = queue_input(s, pkt, s->pkt->data, (size_t)s->pkt->size, 0);
    av_packet_unref(s->pkt);
    return ret < 0 ? ret : 1;
}

/* Return 1 when replay is complete, 0 if waiting for an input buffer, or an error */
static int send_replay(struct nmdi_mediacodec *s)
{
    while (s->replay_pos < s->nb_replay_pkts) {
        const int ret = queue_packet(s, s->replay_pkts[s->replay_pos]);
        if (ret <= 0)
            return ret;
        s->replay_pos++;
    }

    return 1;
}

static int queue_eos(struct nmdi_mediacodec *s)
{
    int ret = send_replay(s);
    if (ret <= 0)
        return ret;

    ret = dequeue_input(s);
    if (ret <= 0)
        return ret;

    ret = queue_input(s, NULL, NULL, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    if (ret < 0)
        return ret;

    s->eos_pending = false;
    return 0;
}

static int send_packet(struct nmdi_mediacodec *s, const AVPacket *pkt)
{
    if (!s->codec)
        return AVERROR_EXTERNAL;

    if (!pkt) {
        s->draining = true;
        s->eos_pending = true;
        return queue_eos(s);
    }

    int ret = send_replay(s);
    if (ret <= 0)
        return ret < 0 ? ret : AVERROR(EAGAIN);

    if (s->need_key_frame) {
        if (!(pkt->flags & AV_PKT_FLAG_KEY))
            return 0;
        s->need_key_frame = false;
    }

    ret = queue_packet(s, pkt);
    if (ret < 0)
        return ret;
    if (!ret)
        return AVERROR(EAGAIN);

    free_old_pkts(s, FREE_PKTS_PER_SEND);

    /* Replaying a packet that exceeded the input buffer would fail again */
    if (!s->need_key_frame)
        record_packet(s, pkt);

    return 0;
}

int nmdi_mediacodec_send(struct nmdi_mediacodec *s, const AVPacket *pkt)
{
    for (;;) {
        int ret = send_packet(s, pkt);
        if (ret != AVERROR_EXTERNAL)
            return ret;

        ret = nmdi_mediacodec_reset(s, "unable to send a packet");
        if (ret < 0)
            return ret;
    }
}

static struct pts_entry get_pts_entry(struct nmdi_mediacodec *s, int64_t us)
{
    for (size_t i = 0; i < MAX_PTS_ENTRIES; i++) {
        struct pts_entry *entry = &s->pts_entries[i];
        if (entry->used && entry->us == us) {
            entry->used = false;
            return *entry;
        }
    }

    return (struct pts_entry){.us = us, .pts = av_rescale_q(us, AV_TIME_BASE_Q, s->time_base)};
}

/* Replaying restores the codec state but must not return the same frames twice */
static bool is_output_again(const struct nmdi_mediacodec *s, int64_t pts)
{
    return s->skip_output && pts != AV_NOPTS_VALUE && pts <= s->last_output_pts;
}

/* Record only frames returned to the caller so failed output can be retried after a reset */
static void set_output(struct nmdi_mediacodec *s, int64_t pts)
{
    s->skip_output = false;
    if (pts != AV_NOPTS_VALUE)
        s->last_output_pts = pts;
}

static int output_video(struct nmdi_mediacodec *s, size_t index, const AMediaCodecBufferInfo *info, AVFrame *frame)
{
    const struct pts_entry entry = get_pts_entry(s, info->presentationTimeUs);
    if (is_output_again(s, entry.pts)) {
        if (AMediaCodec_releaseOutputBuffer(s->codec, index, false) != AMEDIA_OK)
            return AVERROR_EXTERNAL;
        return AVERROR(EAGAIN);
    }

    /* Presentation times may repeat; use a sequence number to identify the rendered image */
    const int64_t seq = ++s->render_seq;
    media_status_t status = AMediaCodec_releaseOutputBufferAtTime(s->codec, index, seq);
    if (status != AMEDIA_OK) {
        LOG(s, ERROR, "unable to render an output buffer: %s (%d)", get_status_str(status), status);
        return AVERROR_EXTERNAL;
    }

    AImage *image;
    int ret = acquire_image(s, seq, &image);
    if (ret == AVERROR(ETIMEDOUT)) {
        LOG(s, WARNING, "frame (pts=%" PRId64 ") not rendered, dropping it", entry.pts);
        return AVERROR(EAGAIN);
    }
    if (ret < 0)
        return ret;

    struct mc_image *img = av_mallocz(sizeof(*img));
    if (!img) {
        AImage_delete(image);
        return AVERROR(ENOMEM);
    }

    img->reader = reader_ref(s->reader);
    img->image = image;
    img->release_fence_fd = -1;

    frame->buf[0] = av_buffer_create((uint8_t *)img, sizeof(*img), free_image, NULL, 0);
    if (!frame->buf[0]) {
        free_image(NULL, (uint8_t *)img);
        return AVERROR(ENOMEM);
    }

    frame->data[3] = (uint8_t *)image;
    frame->format = AV_PIX_FMT_MEDIACODEC;
    frame->width = s->width;
    frame->height = s->height;
    frame->sample_aspect_ratio = s->par->sample_aspect_ratio;
    frame->pts = entry.pts;
    frame->duration = entry.duration;
    frame->color_range = s->color_range;
    frame->color_primaries = s->color_primaries;
    frame->color_trc = s->color_trc;
    frame->colorspace = s->colorspace;

    set_output(s, entry.pts);
    return 0;
}

/*
 * Remove encoder delay and end padding using the container's sample counts.
 * Return AVERROR(EAGAIN) if no samples remain.
 */
static int output_audio(struct nmdi_mediacodec *s, size_t index, const AMediaCodecBufferInfo *info, AVFrame *frame)
{
    const struct pts_entry entry = get_pts_entry(s, info->presentationTimeUs);

    int ret = 0;

    /*
     * The pointer already points to the PCM data; do not add info->offset.
     * Before API 36, buf_size is the buffer capacity, so use info->size for
     * the PCM length and buf_size only as an upper bound.
     */
    size_t buf_size;
    const uint8_t *buf = AMediaCodec_getOutputBuffer(s->codec, index, &buf_size);
    if (!buf || s->sample_fmt == AV_SAMPLE_FMT_NONE || s->nb_channels <= 0 || (size_t)info->size > buf_size) {
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    if (is_output_again(s, entry.pts)) {
        ret = AVERROR(EAGAIN);
        goto end;
    }

    const int sample_size = av_get_bytes_per_sample(s->sample_fmt) * s->nb_channels;
    int nb_samples = info->size / sample_size;
    int start = 0;

    if (entry.skip_samples)
        s->skip_samples = entry.skip_samples;
    if (s->skip_samples > 0) {
        start = (int)FFMIN(s->skip_samples, nb_samples);
        s->skip_samples -= start;
        nb_samples -= start;
    }

    nb_samples -= (int)FFMIN(entry.discard_padding, (uint32_t)nb_samples);
    if (nb_samples <= 0) {
        ret = AVERROR(EAGAIN);
        goto end;
    }

    frame->format = s->sample_fmt;
    frame->sample_rate = s->sample_rate;
    frame->nb_samples = nb_samples;
    av_channel_layout_default(&frame->ch_layout, s->nb_channels);
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        goto end;

    memcpy(frame->data[0], buf + (size_t)start * sample_size, (size_t)nb_samples * sample_size);

    frame->pts = entry.pts;
    if (start && frame->pts != AV_NOPTS_VALUE)
        frame->pts += av_rescale_q(start, (AVRational){1, s->sample_rate}, s->time_base);
    frame->duration = av_rescale_q(nb_samples, (AVRational){1, s->sample_rate}, s->time_base);

end:
    if (AMediaCodec_releaseOutputBuffer(s->codec, index, false) != AMEDIA_OK && ret >= 0)
        ret = AVERROR_EXTERNAL;

    if (ret < 0)
        av_frame_unref(frame);
    else
        set_output(s, entry.pts);

    return ret;
}

static int receive_frame(struct nmdi_mediacodec *s, AVFrame *frame, int64_t timeout_us)
{
    if (!s->codec)
        return AVERROR_EXTERNAL;
    if (s->eos)
        return AVERROR_EOF;

    for (;;) {
        if (s->eos_pending) {
            int ret = queue_eos(s);
            if (ret < 0)
                return ret;
        }

        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(s->codec, &info, timeout_us);
        if (index >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
                s->eos = true;

            if (info.size > 0) {
                const int ret = s->type == AVMEDIA_TYPE_VIDEO ? output_video(s, (size_t)index, &info, frame)
                                                              : output_audio(s, (size_t)index, &info, frame);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return ret;
                if (ret >= 0) {
                    s->nb_resets = 0;
                    return 0;
                }
            } else if (AMediaCodec_releaseOutputBuffer(s->codec, (size_t)index, false) != AMEDIA_OK) {
                return AVERROR_EXTERNAL;
            }

            if (s->eos)
                return AVERROR_EOF;
            continue;
        }

        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            update_format(s);
            continue;
        }

        if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            return AVERROR(EAGAIN);

        LOG(s, ERROR, "unable to dequeue an output buffer: %s (%zd)", get_status_str((media_status_t)index), index);
        return AVERROR_EXTERNAL;
    }
}

int nmdi_mediacodec_receive(struct nmdi_mediacodec *s, AVFrame *frame, int64_t timeout_us)
{
    int ret = receive_frame(s, frame, timeout_us);
    if (ret == AVERROR_EXTERNAL) {
        ret = nmdi_mediacodec_reset(s, "unable to receive a frame");
        if (ret >= 0)
            ret = AVERROR(EAGAIN);
    }

    return ret;
}

void nmdi_mediacodec_flush(struct nmdi_mediacodec *s)
{
    s->draining = false;
    reset_state(s);
    clear_replay(s);
    s->last_output_pts = AV_NOPTS_VALUE;
    s->skip_output = false;
    s->need_key_frame = false;

    /* Allow retrying a failed codec after a seek */
    s->nb_resets = 0;
    if (!s->codec) {
        nmdi_mediacodec_reset(s, "seeking after the decoder failures");
        return;
    }

    const media_status_t status = AMediaCodec_flush(s->codec);
    if (status != AMEDIA_OK) {
        LOG(s, ERROR, "unable to flush the decoder: %s (%d)", get_status_str(status), status);
        nmdi_mediacodec_reset(s, "unable to flush the decoder");
    }
}

void nmdi_mediacodec_freep(struct nmdi_mediacodec **sp)
{
    struct nmdi_mediacodec *s = *sp;
    if (!s)
        return;

    /* Stop the codec before releasing its output surface; held frames keep the reader alive */
    delete_codec(s);
    reader_unrefp(&s->reader);

    clear_replay(s);
    free_old_pkts(s, SIZE_MAX);
    av_freep(&s->replay_pkts);
    av_freep(&s->old_pkts);

    av_bsf_free(&s->bsf);
    av_packet_free(&s->pkt);
    av_freep(&s->codec_name);
    av_freep(sp);
}
