#ifndef SF3500_AUDIO_H
#define SF3500_AUDIO_H

#include <stdint.h>

/* Pre-resampled to HWAUDIO_RATE stereo S16 once at load time - SFX are short,
 * so a one-shot conversion has none of the streaming/drift concerns a
 * continuously-playing source has (see MusicStream below). */
typedef struct {
	int16_t *pcm;
	int frames;
} SfxData;
#define PHL_Sound SfxData*

/* OGG Vorbis decode via the vendored stb_vorbis (same decoder SDL2_mixer's
 * own SDL2MIXER_VORBIS=STB build on this device uses - lifted directly
 * rather than pulling in SDL2_mixer's own Mix_* API, which would drag in a
 * real SDL2 audio-device dependency this backend doesn't need). */
typedef struct MusicStream MusicStream;

typedef struct {
	int loop;
	MusicStream *snd;
} PHL_Music;

extern int music_volume;

void PHL_AudioInit();
void PHL_AudioClose();

void PHL_MusicVolume(float vol);

PHL_Music PHL_LoadMusic(char* fname, int loop);
PHL_Sound PHL_LoadSound(char* fname);

void PHL_PlayMusic(PHL_Music snd);
void PHL_PlaySound(PHL_Sound snd, int channel);

void PHL_StopMusic();
void PHL_StopSound(PHL_Sound snd, int channel);

void PHL_FreeMusic(PHL_Music snd);
void PHL_FreeSound(PHL_Sound snd);

#endif
