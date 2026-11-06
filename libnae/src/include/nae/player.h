/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_PLAYER_H
#define NAE_PLAYER_H
#include "nae.h"

#if defined(_WIN32) && !defined(NAE_STATIC)
# ifdef NAE_PLAYER_BUILD
#  define NAE_PLAYER_API __declspec(dllexport)
# else
#  define NAE_PLAYER_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define NAE_PLAYER_API __attribute__((visibility("default")))
#else
# define NAE_PLAYER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A player of tracks of media files laid on a project, as an editor plays
 * the audio of its timeline: each track plays a part of its media from a
 * frame of the project, through its fader (volume keyframes) and volume,
 * mixed under a master volume. Built on an engine, a transport, the
 * nope.media sources and the miniaudio sink.
 *
 * All the functions are for the control thread; they are serialized, and
 * none waits for the audio thread, but a commit waits for the previous
 * one to be adopted (a callback).
 */

struct nae_player;
struct nae_track;

enum nae_player_mode {
    NAE_PLAYER_DEVICE,       /* plays on the default audio output */
    NAE_PLAYER_NULL_DEVICE,  /* plays on a headless device, in real time */
    NAE_PLAYER_HOST,         /* the host renders in real time with nae_player_render(), on and on once started */
    NAE_PLAYER_OFFLINE,      /* the host renders as fast as it can, every read waiting for its data */
};

struct nae_player_config {
    enum nae_player_mode mode;
    uint32_t sample_rate;    /* 0 for 48000 */
    uint32_t period_ms;      /* of the device, 0 for the backend's default */
};

NAE_PLAYER_API int nae_player_create(const struct nae_player_config *config, struct nae_player **player);
NAE_PLAYER_API void nae_player_freep(struct nae_player **player);
NAE_PLAYER_API uint32_t nae_player_get_sample_rate(const struct nae_player *player);

/* Times are in frames at the sample rate of the player */
struct nae_track_params {
    const char *filename;
    int64_t start;           /* frame of the project the track starts at */
    int64_t trim;            /* frame of the media it starts from */
    int64_t duration;        /* frames played, < 0 for the rest of the media */
    float volume;
    int normalize;           /* level the loudness of the media */
};

/* The track is the player's, until removed */
NAE_PLAYER_API int nae_player_add_track(struct nae_player *player, const struct nae_track_params *params,
                                        struct nae_track **track);
NAE_PLAYER_API int nae_player_set_track_timings(struct nae_player *player, struct nae_track *track,
                                                int64_t start, int64_t trim, int64_t duration);
NAE_PLAYER_API int nae_player_set_track_volume(struct nae_player *player, struct nae_track *track, float volume);
/* Keyframes in frames from the start of the track, copied */
NAE_PLAYER_API int nae_player_set_track_fader(struct nae_player *player, struct nae_track *track,
                                              const struct nae_keyframe *keyframes, uint32_t nb_keyframes);
/* Frames of the media of the track, 0 if unknown */
NAE_PLAYER_API int64_t nae_track_get_length(const struct nae_track *track);
/* Frames the track plays */
NAE_PLAYER_API int64_t nae_track_get_duration(const struct nae_track *track);
NAE_PLAYER_API int nae_player_remove_track(struct nae_player *player, struct nae_track **track);
NAE_PLAYER_API int nae_player_clear_tracks(struct nae_player *player);

NAE_PLAYER_API int nae_player_play(struct nae_player *player);
NAE_PLAYER_API int nae_player_pause(struct nae_player *player);
/* Pauses, back to the start, the device stopped */
NAE_PLAYER_API int nae_player_stop(struct nae_player *player);
NAE_PLAYER_API int nae_player_is_playing(const struct nae_player *player);
NAE_PLAYER_API int nae_player_seek(struct nae_player *player, int64_t frame);
/* The frame rendered next, or the frame of a seek not applied yet */
NAE_PLAYER_API int64_t nae_player_get_position(const struct nae_player *player);
/* The frame heard at a time of nae_sink_now() (playing on a device), or the position */
NAE_PLAYER_API int64_t nae_player_get_position_at(const struct nae_player *player, int64_t time);
NAE_PLAYER_API int nae_player_set_duration(struct nae_player *player, int64_t frames);
NAE_PLAYER_API int64_t nae_player_get_duration(const struct nae_player *player);
NAE_PLAYER_API int nae_player_set_looping(struct nae_player *player, int looping);
NAE_PLAYER_API int nae_player_is_looping(const struct nae_player *player);
/* [start, end), an empty range for the whole duration */
NAE_PLAYER_API int nae_player_set_loop_range(struct nae_player *player, int64_t start, int64_t end);
/* Silence at the end of each pass of the loop, the position staying at its end */
NAE_PLAYER_API int nae_player_set_loop_hold(struct nae_player *player, int64_t frames);
NAE_PLAYER_API int nae_player_set_volume(struct nae_player *player, float volume);
NAE_PLAYER_API float nae_player_get_volume(const struct nae_player *player);

/* NAE_PLAYER_HOST and NAE_PLAYER_OFFLINE: renders `frames` stereo frames
 * from the position, on the thread rendering the player (one at a time).
 * `time` is when the first one is heard, in nanoseconds (0 if unknown). */
NAE_PLAYER_API int nae_player_render(struct nae_player *player, int64_t time, uint32_t frames, float *output);

enum nae_record_format {
    NAE_RECORD_S16,
    NAE_RECORD_F32,
};

/* NAE_PLAYER_OFFLINE: writes [0, duration) of the project to a WAV file */
NAE_PLAYER_API int nae_player_record(struct nae_player *player, const char *filename,
                                     enum nae_record_format format);

struct nae_player_stats {
    uint32_t underruns;      /* device callbacks with audio missing */
    int64_t silent_frames;   /* of the tracks, played as silence, the audio not there in time */
    int64_t skipped_frames;  /* of the tracks, decoded too late */
    int64_t requests;        /* positions asked of the decoders, seeks included */
    int64_t errors;          /* failures of nope.media or of the device */
};

/* From any thread, never waiting: while the player is edited, the counts of
 * the tracks are those of the last call */
NAE_PLAYER_API void nae_player_get_stats(const struct nae_player *player, struct nae_player_stats *stats);

#ifdef __cplusplus
}
#endif
#endif
