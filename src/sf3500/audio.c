#include "audio.h"
#include "hwaudio.h"

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

int music_volume = 4;

/* The actual mix gain, 0..1 - set by PHL_MusicVolume(). Kept SEPARATE from
 * music_volume: that global is upstream's own 0-4 UI/save setting (see
 * options.c's "(music_volume+1)%5" slider and ini.c's musicvolumeLoad) -
 * every call site already converts it to a 0..1 float itself
 * (0.25f * music_volume) before calling PHL_MusicVolume. Writing back into
 * music_volume from inside PHL_MusicVolume - the original bug here - fought
 * the slider's own state and pinned the real gain at max regardless of the
 * UI value. */
static float g_music_gain = 1.0f;

#define NUM_CHANNELS 8

struct MusicStream {
	stb_vorbis *v;
	unsigned char *filebuf;
	int native_rate;
	int native_channels;
	/* Fractional-position FIFO of decoded native-rate samples, so the
	 * resample ratio's fractional part carries exactly across pump calls -
	 * the same class of fix the TinySTG port needed (see
	 * sf3500_port_pipeline_recipe memory): resetting/rounding this fresh
	 * every call is what caused that saga's slow, steady pitch drift. */
	short *fifo;      /* interleaved, native_channels per frame */
	int fifo_cap;
	int fifo_count;    /* frames currently buffered */
	double fifo_pos;   /* fractional read cursor, in frames, into the fifo */
	int eof;
};

typedef struct {
	SfxData *data;
	int pos;      /* frames already played */
	int active;
} Channel;

static Channel g_channels[NUM_CHANNELS];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static MusicStream *g_music = NULL;
static int g_music_loop = 0;

static pthread_t g_thread;
static volatile int g_running = 0;

/* ---- one-shot WAV loader + resample to HWAUDIO_RATE stereo S16 ---- */

static int16_t *resample_to_hw(const int16_t *src, int src_frames, int src_rate, int src_channels, int *out_frames)
{
	int frames = (int)((long long)src_frames * HWAUDIO_RATE / src_rate);
	if (frames < 1) frames = 1;
	int16_t *dst = (int16_t*)malloc((size_t)frames * 2 * sizeof(int16_t));
	double ratio = (double)src_rate / HWAUDIO_RATE;

	for (int i = 0; i < frames; i++) {
		double pos = i * ratio;
		int i0 = (int)pos;
		double frac = pos - i0;
		int i1 = i0 + 1;
		if (i1 >= src_frames) i1 = src_frames - 1;
		if (i0 >= src_frames) i0 = src_frames - 1;

		if (src_channels == 1) {
			short a = src[i0], b = src[i1];
			short s = (short)(a + (b - a) * frac);
			dst[i * 2] = s;
			dst[i * 2 + 1] = s;
		} else {
			short al = src[i0 * 2],     ar = src[i0 * 2 + 1];
			short bl = src[i1 * 2],     br = src[i1 * 2 + 1];
			dst[i * 2]     = (short)(al + (bl - al) * frac);
			dst[i * 2 + 1] = (short)(ar + (br - ar) * frac);
		}
	}

	*out_frames = frames;
	return dst;
}

PHL_Sound PHL_LoadSound(char* fname)
{
	char path[4096];
	snprintf(path, sizeof(path), "data/%s", fname);

	FILE *f = fopen(path, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = (unsigned char*)malloc((size_t)fsize);
	fread(buf, 1, (size_t)fsize, f);
	fclose(f);

	/* Canonical RIFF/WAVE: walk chunks to find 'fmt ' and 'data' rather than
	 * assuming fixed offsets - these assets are plain PCM but a generic walk
	 * costs nothing and isn't tied to one specific exporter's layout. */
	if (fsize < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
		free(buf);
		return NULL;
	}

	int channels = 1, rate = 44100, bits = 16;
	const unsigned char *data = NULL;
	long data_len = 0;

	long off = 12;
	while (off + 8 <= fsize) {
		char id[5] = {0};
		memcpy(id, buf + off, 4);
		unsigned int csize;
		memcpy(&csize, buf + off + 4, 4);
		long body = off + 8;

		if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= fsize) {
			memcpy(&channels, buf + body + 2, 2);
			channels &= 0xFFFF;
			memcpy(&rate, buf + body + 4, 4);
			memcpy(&bits, buf + body + 14, 2);
			bits &= 0xFFFF;
		} else if (memcmp(id, "data", 4) == 0) {
			data = buf + body;
			data_len = csize;
		}

		off = body + csize + (csize & 1);
	}

	if (!data || bits != 16) {
		free(buf);
		return NULL;
	}

	int src_frames = (int)(data_len / (2 * channels));
	int out_frames;
	int16_t *resampled = resample_to_hw((const int16_t*)data, src_frames, rate, channels, &out_frames);
	free(buf);

	SfxData *sfx = (SfxData*)malloc(sizeof(SfxData));
	sfx->pcm = resampled;
	sfx->frames = out_frames;
	return sfx;
}

void PHL_FreeSound(PHL_Sound snd)
{
	if (!snd) return;
	free(snd->pcm);
	free(snd);
}

void PHL_PlaySound(PHL_Sound snd, int channel)
{
	if (!snd || channel < 0 || channel >= NUM_CHANNELS) return;
	pthread_mutex_lock(&g_mtx);
	g_channels[channel].data = snd;
	g_channels[channel].pos = 0;
	g_channels[channel].active = 1;
	pthread_mutex_unlock(&g_mtx);
}

void PHL_StopSound(PHL_Sound snd, int channel)
{
	(void)snd;
	if (channel < 0 || channel >= NUM_CHANNELS) return;
	pthread_mutex_lock(&g_mtx);
	g_channels[channel].active = 0;
	pthread_mutex_unlock(&g_mtx);
}

/* ---- streaming OGG music via stb_vorbis ---- */

PHL_Music PHL_LoadMusic(char* fname, int loop)
{
	PHL_Music ret;
	ret.loop = loop;
	ret.snd = NULL;

	char path[4096];
	snprintf(path, sizeof(path), "data/%s.ogg", fname);

	FILE *f = fopen(path, "rb");
	if (!f) return ret;
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = (unsigned char*)malloc((size_t)fsize);
	fread(buf, 1, (size_t)fsize, f);
	fclose(f);

	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(buf, (int)fsize, &err, NULL);
	if (!v) {
		free(buf);
		return ret;
	}

	stb_vorbis_info info = stb_vorbis_get_info(v);

	MusicStream *m = (MusicStream*)malloc(sizeof(MusicStream));
	m->v = v;
	m->filebuf = buf;
	m->native_rate = (int)info.sample_rate;
	m->native_channels = info.channels;
	m->fifo_cap = m->native_rate; /* 1s of headroom, resized if ever needed */
	m->fifo = (short*)malloc((size_t)m->fifo_cap * m->native_channels * sizeof(short));
	m->fifo_count = 0;
	m->fifo_pos = 0.0;
	m->eof = 0;

	ret.snd = m;
	return ret;
}

void PHL_FreeMusic(PHL_Music snd)
{
	if (!snd.snd) return;
	pthread_mutex_lock(&g_mtx);
	if (g_music == snd.snd) g_music = NULL;
	pthread_mutex_unlock(&g_mtx);

	stb_vorbis_close(snd.snd->v);
	free(snd.snd->filebuf);
	free(snd.snd->fifo);
	free(snd.snd);
}

void PHL_PlayMusic(PHL_Music snd)
{
	if (!snd.snd) return;
	pthread_mutex_lock(&g_mtx);
	stb_vorbis_seek_start(snd.snd->v);
	snd.snd->fifo_count = 0;
	snd.snd->fifo_pos = 0.0;
	snd.snd->eof = 0;
	g_music = snd.snd;
	g_music_loop = snd.loop;
	pthread_mutex_unlock(&g_mtx);
}

void PHL_StopMusic()
{
	pthread_mutex_lock(&g_mtx);
	g_music = NULL;
	pthread_mutex_unlock(&g_mtx);
}

void PHL_MusicVolume(float vol)
{
	if (vol < 0.0f) vol = 0.0f;
	if (vol > 1.0f) vol = 1.0f;
	g_music_gain = vol;
}

/* Tops up m->fifo with freshly decoded native-rate frames, looping/seeking
 * as needed, up to `want_frames` buffered ahead of fifo_pos. Must be called
 * with g_mtx held. */
static void music_fill(MusicStream *m, int want_frames)
{
	if (want_frames > m->fifo_cap) {
		int old_cap = m->fifo_cap;
		m->fifo_cap = want_frames + m->native_rate / 4;
		m->fifo = (short*)realloc(m->fifo, (size_t)m->fifo_cap * m->native_channels * sizeof(short));
		(void)old_cap;
	}

	while (m->fifo_count < want_frames && !m->eof) {
		int space = m->fifo_cap - m->fifo_count;
		int got = stb_vorbis_get_samples_short_interleaved(
			m->v, m->native_channels,
			m->fifo + (size_t)m->fifo_count * m->native_channels,
			space * m->native_channels);
		m->fifo_count += got;
		if (got == 0) {
			if (g_music_loop) {
				stb_vorbis_seek_start(m->v);
			} else {
				m->eof = 1;
			}
		}
	}
}

/* Consumes whole frames from the front of the fifo once fifo_pos has
 * advanced past them, so the fifo doesn't grow without bound. */
static void music_drain_consumed(MusicStream *m)
{
	int consumed = (int)m->fifo_pos;
	if (consumed <= 0) return;
	if (consumed > m->fifo_count) consumed = m->fifo_count;
	int remaining = m->fifo_count - consumed;
	if (remaining > 0) {
		memmove(m->fifo, m->fifo + (size_t)consumed * m->native_channels,
		        (size_t)remaining * m->native_channels * sizeof(short));
	}
	m->fifo_count = remaining;
	m->fifo_pos -= consumed;
}

/* Resamples `frames` output frames (at HWAUDIO_RATE) from m into `out`
 * (stereo S16, additively mixed - caller has already zeroed/filled it). */
static void music_mix(MusicStream *m, int16_t *out, int frames)
{
	double ratio = (double)m->native_rate / HWAUDIO_RATE;
	int need = (int)(m->fifo_pos + frames * ratio) + 2;
	music_fill(m, need);

	float gain = g_music_gain;
	for (int i = 0; i < frames; i++) {
		double pos = m->fifo_pos + i * ratio;
		int i0 = (int)pos;
		double frac = pos - i0;
		int i1 = i0 + 1;
		if (i0 >= m->fifo_count) i0 = m->fifo_count > 0 ? m->fifo_count - 1 : 0;
		if (i1 >= m->fifo_count) i1 = i0;

		short l0, r0, l1, r1;
		if (m->fifo_count == 0) {
			l0 = r0 = l1 = r1 = 0;
		} else if (m->native_channels == 1) {
			l0 = r0 = m->fifo[i0];
			l1 = r1 = m->fifo[i1];
		} else {
			l0 = m->fifo[i0 * 2];     r0 = m->fifo[i0 * 2 + 1];
			l1 = m->fifo[i1 * 2];     r1 = m->fifo[i1 * 2 + 1];
		}

		int l = (int)(l0 + (l1 - l0) * frac);
		int r = (int)(r0 + (r1 - r0) * frac);
		int ol = out[i * 2]     + (int)(l * gain);
		int or_ = out[i * 2 + 1] + (int)(r * gain);
		if (ol > 32767) ol = 32767; if (ol < -32768) ol = -32768;
		if (or_ > 32767) or_ = 32767; if (or_ < -32768) or_ = -32768;
		out[i * 2]     = (int16_t)ol;
		out[i * 2 + 1] = (int16_t)or_;
	}

	m->fifo_pos += frames * ratio;
	music_drain_consumed(m);
}

static void sfx_mix(int16_t *out, int frames)
{
	for (int c = 0; c < NUM_CHANNELS; c++) {
		Channel *ch = &g_channels[c];
		if (!ch->active || !ch->data) continue;

		int avail = ch->data->frames - ch->pos;
		int n = frames < avail ? frames : avail;
		const int16_t *src = ch->data->pcm + (size_t)ch->pos * 2;

		for (int i = 0; i < n; i++) {
			int l = out[i * 2]     + src[i * 2];
			int r = out[i * 2 + 1] + src[i * 2 + 1];
			if (l > 32767) l = 32767; if (l < -32768) l = -32768;
			if (r > 32767) r = 32767; if (r < -32768) r = -32768;
			out[i * 2]     = (int16_t)l;
			out[i * 2 + 1] = (int16_t)r;
		}

		ch->pos += n;
		if (ch->pos >= ch->data->frames) ch->active = 0;
	}
}

/* ---- pump thread: drift-free wall-clock target -> mix -> hwaudio_write.
 * Same shape as the final, proven TinySTG architecture (see
 * sf3500_port_pipeline_recipe memory): a cumulative output-frame counter
 * tied to real elapsed time, capped backlog sized from hwaudio.h's own
 * shared ring constant, and the ring's honest accepted-count return value
 * is what the counter advances by - not the requested amount. */

#define MAX_OUT_BATCH HWAUDIO_RING_SOFT_CAP

static int16_t *g_mixbuf = NULL;

static void *audio_pump_thread(void *unused)
{
	(void)unused;
	struct timespec start, next_wake;
	long long out_frames_produced = 0;
	const long period_ns = 20000000L; /* wake every 20ms */

	clock_gettime(CLOCK_MONOTONIC, &start);
	next_wake = start;

	g_mixbuf = (int16_t*)malloc((size_t)MAX_OUT_BATCH * 2 * sizeof(int16_t));

	while (g_running) {
		next_wake.tv_nsec += period_ns;
		if (next_wake.tv_nsec >= 1000000000L) {
			next_wake.tv_nsec -= 1000000000L;
			next_wake.tv_sec += 1;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long long elapsed_ns = (long long)(now.tv_sec - start.tv_sec) * 1000000000LL
		                      + (now.tv_nsec - start.tv_nsec);
		long long out_target = elapsed_ns * HWAUDIO_RATE / 1000000000LL;

		long long want_ll = out_target - out_frames_produced;
		if (want_ll > MAX_OUT_BATCH) {
			/* Never chase an unbounded backlog - drop it, same philosophy
			 * as hwaudio_write's own soft cap. */
			out_frames_produced = out_target - MAX_OUT_BATCH;
			want_ll = MAX_OUT_BATCH;
		}
		if (want_ll <= 0) continue;
		int want = (int)want_ll;

		memset(g_mixbuf, 0, (size_t)want * 2 * sizeof(int16_t));

		pthread_mutex_lock(&g_mtx);
		if (g_music) music_mix(g_music, g_mixbuf, want);
		sfx_mix(g_mixbuf, want);
		pthread_mutex_unlock(&g_mtx);

		int accepted = hwaudio_write(g_mixbuf, want);
		out_frames_produced += accepted;
	}

	free(g_mixbuf);
	g_mixbuf = NULL;
	return NULL;
}

void PHL_AudioInit()
{
	memset(g_channels, 0, sizeof(g_channels));
	hwaudio_init();
	g_running = 1;
	pthread_create(&g_thread, NULL, audio_pump_thread, NULL);
	PHL_MusicVolume(0.25f * music_volume);
}

void PHL_AudioClose()
{
	g_running = 0;
	pthread_join(g_thread, NULL);
	hwaudio_deinit();
}
