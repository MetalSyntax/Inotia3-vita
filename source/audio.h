/*
 * @file  audio.h
 * @brief Fase 7: real playback for Inotia 3's Com2uS SOUNDSYSTEM_ / SOUNDPLAYER_
 *        surface, which reaches Java/us through 9 static WrapperJinterface
 *        methods (see PORTING_PLAN.md section 0 and source/java.c ids 27-35):
 *
 *          SoundCreate(String path, boolean loop): int      -> handle (>0), 0 on failure
 *          SoundPrepare(int handle): int
 *          SoundPlay(int handle, boolean loop): int
 *          SoundPause(int handle): int
 *          SoundResume(int handle): int
 *          SoundStop(int handle): int
 *          SoundSetVolume(int handle, int volume): int      -> volume 0..100
 *          SoundGetVolume(int handle): int
 *          SoundDestroy(int handle): int
 *
 * Unlike Zenonia's "fire by numeric resource id" model, this is a real
 * handle-per-sound-object API (closer to Android MediaPlayer/SoundPool) --
 * the engine can keep many Sound objects alive (one per distinct SFX/BGM
 * asset it preloads) even though only a few play at once. So handle
 * bookkeeping (path/loop/volume/state) and actual decode voices are two
 * separate pools here: MAX_SOUND_HANDLES cheap metadata slots, backed by a
 * much smaller MAX_ACTIVE_VOICES pool of real OggVorbis_File decoders that
 * get assigned (and stolen, oldest-first, if exhausted) on SoundPlay.
 *
 * Confirmed the only real audio format shipped is OGG/Vorbis (2026-07-21):
 * decompiled_apk/resources/assets/common/game_res/ has files named
 * NAME.ogg.jpg that are literal OGG files (checked via `file`/`xxd`, real
 * "OggS"/vorbis magic) -- APK asset
 * packaging renames already-compressed files with a disguise extension
 * (.jpg) so aapt stores them uncompressed instead of re-deflating them; the
 * ".jpg" is not a real image and must be stripped when staging assets onto
 * the Vita (asset-preparation concern, not something this code handles). No
 * .mmf files exist anywhere in the actual asset bundle, so the .mmf side of
 * SOUNDSYSTEM_* is dead code for this build -- not implemented here.
 */

#ifndef INOTIA3_AUDIO_H
#define INOTIA3_AUDIO_H

void audio_init(void);

int audio_sound_create(const char *path, int loop);
int audio_sound_prepare(int handle);
int audio_sound_play(int handle, int loop);
int audio_sound_pause(int handle);
int audio_sound_resume(int handle);
int audio_sound_stop(int handle);
int audio_sound_destroy(int handle);
int audio_sound_set_volume(int handle, int volume);
int audio_sound_get_volume(int handle);

#endif // INOTIA3_AUDIO_H
