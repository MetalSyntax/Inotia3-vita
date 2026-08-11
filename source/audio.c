/*
 * @file  audio.c
 * @brief See audio.h. Two-tier handle/voice pool feeding one sceAudioOut BGM
 *        port at 44100 Hz stereo, mixed by a dedicated thread. The resample
 *        (for .ogg files not already at 44100 Hz) and mixer clipping logic
 *        below are the same linear-resample approach validated on real
 *        hardware in ../Zenonia3-vita/loader/audio.c -- adapted here from
 *        Zenonia's "play by numeric resource id" model to Inotia 3's real
 *        handle-per-Sound-object JNI surface (see audio.h).
 */

#include "audio.h"
#include "utils/logger.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <tremor/ivorbisfile.h>

#define AUDIO_RATE  44100
#define AUDIO_GRAIN 512

// The mixer thread (audio_thread, below) holds audio_mutex while it calls
// ov_read() on whichever voice it's decoding -- a blocking file read against
// the SAME storage device every other file access on this console shares
// (ux0:), which can stall for an unbounded time under I/O pressure (e.g. a
// stage transition's burst of asset loads). Every *other* audio_sound_*()
// call below runs on the game's own main thread (invoked synchronously from
// a Sound JNI call) and needs this same mutex just to touch a few struct
// fields -- if it waited forever for a mixer thread stuck mid-read, the
// ENTIRE game (input, rendering, logging -- all on that one main thread)
// would hang right along with it. A bounded wait here can never eliminate a
// slow read, but it guarantees the main thread always comes back, at worst
// dropping/delaying one Sound call, instead of freezing the whole game.
#define AUDIO_LOCK_TIMEOUT_US 100000

#define MAX_SOUND_HANDLES 256  // cheap metadata slots -- engine may preload many Sound objects
#define MAX_ACTIVE_VOICES 16   // real concurrent Vorbis decoders (voice-stolen oldest-first)

typedef enum {
    HANDLE_FREE = 0,
    HANDLE_IDLE,     // created, never played (or stopped)
    HANDLE_PLAYING,
    HANDLE_PAUSED,
} handle_state_t;

typedef struct {
    handle_state_t state;
    char path[192];
    int loop;
    int volume;      // 0..100, last value SoundSetVolume set (default 100)
    int voice_idx;   // index into `voices`, -1 if not currently backed by a decoder
} sound_handle_t;

typedef struct {
    OggVorbis_File vf;
    int active;
    int paused;
    int owner_handle;  // 1-based handle index this voice belongs to, for EOF/steal bookkeeping
    int loop;
    int channels;
    long rate;
    float gain;
    uint64_t start_time;
    // Linear resample state, only used when rate != AUDIO_RATE (see voice_decode).
    float pos_frac;
    int16_t prev_l, prev_r;
    int have_prev;
    // Per-voice decode staging -- a shared buffer would drop frames while the
    // mixer alternates between voices.
    int16_t stage[256];
    int stage_frames, stage_pos;

    // Async-open request state -- see audio_thread_process_pending_opens().
    // audio_sound_play() used to fopen()+ov_open() the file itself, on the
    // main thread, before ever touching audio_mutex: a plain blocking read
    // against the same ux0: storage every other file access shares, with NO
    // timeout at all (unlike the mutex wait right below it). Under I/O
    // pressure -- exactly what a stage transition's burst of new asset loads
    // creates -- that open can stall for an unbounded time and freeze the
    // ENTIRE main thread (input, render, logging), which matches the
    // still-freezes-on-stage-change report even after the mutex-timeout fix.
    // Fix: audio_sound_play() only stakes out a voice slot and hands the
    // real (potentially slow) fopen/ov_open off to the mixer thread, which
    // already owns blocking audio I/O by design. `open_token` guards against
    // this slot being stolen/replayed/stopped while its open is still in
    // flight -- the mixer re-checks it (and that the owning handle still
    // claims this exact slot) before installing, and safely discards a
    // stale result otherwise.
    volatile int opening;
    int open_token;
    char pending_path[192];
    int pending_loop;
} voice_t;

static sound_handle_t handles[MAX_SOUND_HANDLES];
static voice_t voices[MAX_ACTIVE_VOICES];
static SceUID audio_mutex = -1;
static SceUID audio_thread_id = -1;
static int audio_port = -1;
static volatile int audio_running = 0;
static volatile int g_open_token_counter = 0;

static uint64_t audio_now_ms(void) {
    return (uint64_t)(sceKernelGetProcessTimeWide() / 1000);
}

// Caller must hold audio_mutex.
static void voice_close(voice_t *v) {
    if (v->active) {
        ov_clear(&v->vf); // also closes the underlying FILE*
        v->active = 0;
        v->paused = 0;
        v->owner_handle = -1;
    }
}

// Detach whatever handle currently owns this voice (natural EOF, stop, or
// being stolen for a different handle) -- caller must hold audio_mutex.
static void voice_detach_owner(voice_t *v) {
    if (v->owner_handle >= 1 && v->owner_handle <= MAX_SOUND_HANDLES) {
        sound_handle_t *h = &handles[v->owner_handle - 1];
        if (h->voice_idx == (int)(v - voices)) {
            h->voice_idx = -1;
            if (h->state == HANDLE_PLAYING || h->state == HANDLE_PAUSED) {
                h->state = HANDLE_IDLE;
            }
        }
    }
}

// Reads one source frame (L,R); returns 0 on definitive EOF (voice closed
// and detached from its handle).
static int voice_next_src_frame(voice_t *v, int16_t *l, int16_t *r) {
    while (v->stage_pos >= v->stage_frames) {
        int bs;
        long got = ov_read(&v->vf, (char *) v->stage, sizeof(v->stage), &bs);
        if (got <= 0) {
            if (v->loop && got == 0 && ov_pcm_seek(&v->vf, 0) == 0)
                continue;
            voice_detach_owner(v);
            voice_close(v);
            return 0;
        }
        v->stage_frames = (int) got / (v->channels * 2);
        v->stage_pos = 0;
    }

    *l = v->stage[v->stage_pos * v->channels];
    *r = v->stage[v->stage_pos * v->channels + (v->channels > 1 ? 1 : 0)];
    v->stage_pos++;
    return 1;
}

// Decodes up to `frames` stereo frames at AUDIO_RATE into `out` (interleaved
// LR), linearly resampling if this voice's real rate differs.
static int voice_decode(voice_t *v, int16_t *out, int frames) {
    int done = 0;

    if (v->rate == AUDIO_RATE) {
        int16_t l, r;
        while (done < frames && v->active) {
            if (!voice_next_src_frame(v, &l, &r)) break;
            out[done * 2]     = (int16_t)(l * v->gain);
            out[done * 2 + 1] = (int16_t)(r * v->gain);
            done++;
        }
        return done;
    }

    float step = (float) v->rate / (float) AUDIO_RATE;
    while (done < frames && v->active) {
        if (!v->have_prev) {
            if (!voice_next_src_frame(v, &v->prev_l, &v->prev_r)) break;
            v->have_prev = 1;
            v->pos_frac = 0.0f;
        }
        int16_t nl = v->prev_l, nr = v->prev_r;
        while (v->pos_frac >= 1.0f) {
            if (!voice_next_src_frame(v, &nl, &nr)) { v->have_prev = 0; break; }
            v->prev_l = nl; v->prev_r = nr;
            v->pos_frac -= 1.0f;
        }
        if (!v->active || !v->have_prev) break;

        out[done * 2]     = (int16_t)(v->prev_l * v->gain);
        out[done * 2 + 1] = (int16_t)(v->prev_r * v->gain);
        done++;
        v->pos_frac += step;
    }
    return done;
}

// Runs on the mixer thread only, once per loop iteration, before mixing.
// Snapshots every voice slot currently flagged `opening` (copying out just
// the fields needed to open the file) while briefly holding audio_mutex,
// then does the actual fopen()+ov_open() for each OUTSIDE the mutex -- so a
// slow/stalled open here can never block a Sound* call coming in from the
// main thread. Re-validates each slot's `open_token` (and that the owning
// handle still claims this exact slot) before installing, since the slot
// may have been stolen, replayed, or stopped while its open was in flight.
static void audio_thread_process_pending_opens(void) {
    struct {
        int slot;
        int token;
        char path[192];
        int loop;
    } reqs[MAX_ACTIVE_VOICES];
    int n = 0;

    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        voice_t *v = &voices[i];
        if (v->opening) {
            reqs[n].slot = i;
            reqs[n].token = v->open_token;
            strncpy(reqs[n].path, v->pending_path, sizeof(reqs[n].path) - 1);
            reqs[n].path[sizeof(reqs[n].path) - 1] = '\0';
            reqs[n].loop = v->pending_loop;
            n++;
        }
    }
    sceKernelUnlockMutex(audio_mutex, 1);

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(reqs[i].path, "rb");
        OggVorbis_File vf;
        int ok = 0;
        int ch = 0;
        long rate = 0;

        if (!f) {
            l_warn("[Audio] async open: \"%s\" not found", reqs[i].path);
        } else if (ov_open(f, &vf, NULL, 0) < 0) {
            l_warn("[Audio] async open: \"%s\" is not a valid Ogg/Vorbis stream", reqs[i].path);
            fclose(f);
        } else {
            vorbis_info *vi = ov_info(&vf, -1);
            if (!vi || (vi->channels != 1 && vi->channels != 2)) {
                l_warn("[Audio] async open: unexpected channel count in \"%s\"", reqs[i].path);
                ov_clear(&vf);
            } else {
                ch = vi->channels;
                rate = vi->rate;
                ok = 1;
            }
        }

        sceKernelLockMutex(audio_mutex, 1, NULL);
        voice_t *v = &voices[reqs[i].slot];
        int still_owned = 0;
        sound_handle_t *h = NULL;
        if (v->opening && v->open_token == reqs[i].token &&
            v->owner_handle >= 1 && v->owner_handle <= MAX_SOUND_HANDLES) {
            h = &handles[v->owner_handle - 1];
            still_owned = (h->state != HANDLE_FREE && h->voice_idx == reqs[i].slot);
            v->opening = 0; // this request is resolved one way or another
        }

        if (ok && still_owned) {
            v->vf = vf;
            v->channels = ch;
            v->rate = rate;
            v->loop = reqs[i].loop;
            v->gain = h->volume / 100.0f; // live value -- a SetVolume during the open window must win
            v->pos_frac = 0.0f;
            v->have_prev = 0;
            v->stage_frames = 0;
            v->stage_pos = 0;
            // NOT resetting v->paused here: a Pause() during the open window
            // already set it, and the mix loop already skips inactive
            // voices regardless -- clobbering it would drop that request.
            v->start_time = audio_now_ms();
            v->active = 1;
        } else {
            if (ok) ov_clear(&vf); // opened fine but no longer wanted -- discard
            if (still_owned) {
                // Wanted, but the open itself failed -- don't leave the
                // handle believing it's still PLAYING with no voice behind it.
                h->voice_idx = -1;
                if (h->state == HANDLE_PLAYING) h->state = HANDLE_IDLE;
            }
        }
        sceKernelUnlockMutex(audio_mutex, 1);
    }
}

static int audio_thread(SceSize args, void *argp) {
    static int16_t mix[AUDIO_GRAIN * 2];
    static int16_t buf[AUDIO_GRAIN * 2];

    while (audio_running) {
        audio_thread_process_pending_opens();

        memset(mix, 0, sizeof(mix));

        sceKernelLockMutex(audio_mutex, 1, NULL);
        for (int vi = 0; vi < MAX_ACTIVE_VOICES; vi++) {
            voice_t *v = &voices[vi];
            if (!v->active || v->paused) continue;
            int got = voice_decode(v, buf, AUDIO_GRAIN);
            for (int i = 0; i < got * 2; i++) {
                int s = mix[i] + buf[i];
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                mix[i] = (int16_t) s;
            }
        }
        sceKernelUnlockMutex(audio_mutex, 1);

        sceAudioOutOutput(audio_port, mix); // blocks until hardware consumed the previous block
    }
    return 0;
}

void audio_init(void) {
    memset(handles, 0, sizeof(handles));
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) voices[i].owner_handle = -1;

    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN,
                                     AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        l_error("[Audio] sceAudioOutOpenPort failed: 0x%08x", audio_port);
        return;
    }

    audio_mutex = sceKernelCreateMutex("inotia3_audio_mutex", 0, 0, NULL);
    audio_running = 1;
    audio_thread_id = sceKernelCreateThread("inotia3_audio", audio_thread,
                                            0x10000100, 0x10000, 0, 0, NULL);
    if (audio_thread_id >= 0) {
        sceKernelStartThread(audio_thread_id, 0, NULL);
        l_success("[Audio] mixer started (port=%d, %d Hz)", audio_port, AUDIO_RATE);
    }
}

int audio_sound_create(const char *path, int loop) {
    if (!path) return 0;

    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundCreate(\"%s\"): timed out waiting for audio_mutex -- mixer thread busy", path);
        return 0;
    }
    int idx = -1;
    for (int i = 0; i < MAX_SOUND_HANDLES; i++) {
        if (handles[i].state == HANDLE_FREE) { idx = i; break; }
    }
    if (idx < 0) {
        sceKernelUnlockMutex(audio_mutex, 1);
        l_error("[Audio] SoundCreate(\"%s\"): out of handle slots (max %d)", path, MAX_SOUND_HANDLES);
        return 0;
    }

    sound_handle_t *h = &handles[idx];
    memset(h, 0, sizeof(*h));
    strncpy(h->path, path, sizeof(h->path) - 1);
    h->loop = loop;
    h->volume = 100;
    h->voice_idx = -1;
    h->state = HANDLE_IDLE;
    sceKernelUnlockMutex(audio_mutex, 1);

    l_debug("[Audio] SoundCreate(\"%s\", loop=%d) -> handle %d", path, loop, idx + 1);
    return idx + 1;
}

// Best-effort existence check -- doesn't hold a persistent voice open, and
// no longer does a full fopen()+ov_open() round trip either: that was a
// second untimed blocking-I/O call site on the main thread alongside
// audio_sound_play()'s (see voice_t's own comment on `opening`/`open_token`
// above). A plain stat() is the same "does this path exist" primitive
// source/java.c's own inotia3_resolve_sound_path() already relies on for
// this exact storage -- real decode-validity is still fully checked, just
// asynchronously, by audio_sound_play()'s own open path.
int audio_sound_prepare(int handle) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return -1;
    sound_handle_t *h = &handles[handle - 1];
    if (h->state == HANDLE_FREE) return -1;

    struct stat st;
    if (stat(h->path, &st) != 0) {
        l_warn("[Audio] SoundPrepare(%d): \"%s\" not found", handle, h->path);
        return -1;
    }
    return 0;
}

// Stakes out a voice slot and hands the actual fopen()+ov_open() off to the
// mixer thread (audio_thread_process_pending_opens()) instead of doing it
// here on the main thread -- see voice_t's own comment for why. Everything
// this function itself does is struct-only and fast, bounded by the same
// AUDIO_LOCK_TIMEOUT_US mutex wait already used everywhere else in this
// file; it returns before the file is actually opened, so a slow/stalled
// open can, at worst, delay this one sound starting -- never the caller.
int audio_sound_play(int handle, int loop) {
    if (audio_port < 0 || handle < 1 || handle > MAX_SOUND_HANDLES) return -1;

    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundPlay(%d): timed out waiting for audio_mutex -- mixer thread busy", handle);
        return -1;
    }

    sound_handle_t *h = &handles[handle - 1];
    if (h->state == HANDLE_FREE) {
        sceKernelUnlockMutex(audio_mutex, 1);
        return -1;
    }

    // Reuse this handle's own voice slot if it already has one; otherwise
    // grab a free slot (not currently playing AND not already mid-open), or
    // steal the oldest slot (simple LRU -- 16 concurrent streams is generous
    // for a 2D RPG, but the engine may preload far more Sound *handles* than
    // can ever play at once). Stealing a slot that's itself still mid-open
    // is safe: its `open_token` no longer matches once we overwrite it
    // below, so audio_thread_process_pending_opens() detects that and
    // discards the stale result when it eventually resolves.
    int slot = h->voice_idx;
    if (slot < 0) {
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            if (!voices[i].active && !voices[i].opening) { slot = i; break; }
        }
    }
    if (slot < 0) {
        uint64_t oldest = (uint64_t) -1;
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            if (voices[i].start_time < oldest) { oldest = voices[i].start_time; slot = i; }
        }
        voice_detach_owner(&voices[slot]);
    }

    voice_t *v = &voices[slot];
    voice_close(v); // no-op unless this slot already had a fully-open decoder
    strncpy(v->pending_path, h->path, sizeof(v->pending_path) - 1);
    v->pending_path[sizeof(v->pending_path) - 1] = '\0';
    v->pending_loop = loop;
    v->owner_handle = handle;
    v->open_token = ++g_open_token_counter;
    v->start_time = audio_now_ms();
    v->opening = 1;

    h->loop = loop;
    h->voice_idx = slot;
    h->state = HANDLE_PLAYING;

    sceKernelUnlockMutex(audio_mutex, 1);
    return 0; // queued -- the mixer thread opens the file asynchronously
}

int audio_sound_pause(int handle) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return -1;
    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundPause(%d): timed out waiting for audio_mutex -- mixer thread busy", handle);
        return -1;
    }
    sound_handle_t *h = &handles[handle - 1];
    if (h->state == HANDLE_PLAYING && h->voice_idx >= 0) {
        voices[h->voice_idx].paused = 1;
        h->state = HANDLE_PAUSED;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
    return 0;
}

int audio_sound_resume(int handle) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return -1;
    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundResume(%d): timed out waiting for audio_mutex -- mixer thread busy", handle);
        return -1;
    }
    sound_handle_t *h = &handles[handle - 1];
    if (h->state == HANDLE_PAUSED && h->voice_idx >= 0) {
        voices[h->voice_idx].paused = 0;
        h->state = HANDLE_PLAYING;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
    return 0;
}

int audio_sound_stop(int handle) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return -1;
    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundStop(%d): timed out waiting for audio_mutex -- mixer thread busy", handle);
        return -1;
    }
    sound_handle_t *h = &handles[handle - 1];
    if (h->voice_idx >= 0) {
        voice_close(&voices[h->voice_idx]);
        h->voice_idx = -1;
    }
    if (h->state != HANDLE_FREE) h->state = HANDLE_IDLE;
    sceKernelUnlockMutex(audio_mutex, 1);
    return 0;
}

int audio_sound_destroy(int handle) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return -1;
    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundDestroy(%d): timed out waiting for audio_mutex -- mixer thread busy", handle);
        return -1;
    }
    sound_handle_t *h = &handles[handle - 1];
    if (h->voice_idx >= 0) {
        voice_close(&voices[h->voice_idx]);
        h->voice_idx = -1;
    }
    h->state = HANDLE_FREE;
    sceKernelUnlockMutex(audio_mutex, 1);
    return 0;
}

int audio_sound_set_volume(int handle, int volume) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return -1;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    unsigned int timeout = AUDIO_LOCK_TIMEOUT_US;
    if (sceKernelLockMutex(audio_mutex, 1, &timeout) < 0) {
        l_warn("[Audio] SoundSetVolume(%d): timed out waiting for audio_mutex -- mixer thread busy", handle);
        return -1;
    }
    sound_handle_t *h = &handles[handle - 1];
    h->volume = volume;
    if (h->voice_idx >= 0) {
        voices[h->voice_idx].gain = volume / 100.0f;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
    return 0;
}

int audio_sound_get_volume(int handle) {
    if (handle < 1 || handle > MAX_SOUND_HANDLES) return 100;
    sound_handle_t *h = &handles[handle - 1];
    if (h->state == HANDLE_FREE) return 100;
    return h->volume;
}
