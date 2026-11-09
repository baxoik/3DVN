#include "audio.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wav.h"

#define MUSIC_CHANNEL 0
#define SFX_FIRST 1
#define SFX_SLOTS 4
#define STREAM_BUFFERS 4
#define STREAM_BYTES (32 * 1024)
#define MAX_SFX_BYTES (2 * 1024 * 1024)

typedef struct {
	int active;
	FILE *file;
	WavInfo info;
	void *memory;
	ndspWaveBuf buffers[STREAM_BUFFERS];
	long position;
	float volume, target, rate;
	int stopping;
} Music;

typedef struct {
	void *memory;
	ndspWaveBuf buffer;
} Sfx;

static int ready;
static Music music;
static Sfx sfx[SFX_SLOTS];
static int next_sfx;

static u16 ndsp_format(const WavInfo *info)
{
	if (info->channels == 1) {
		return info->bits == 8 ? NDSP_FORMAT_MONO_PCM8 : NDSP_FORMAT_MONO_PCM16;
	}
	return info->bits == 8 ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_STEREO_PCM16;
}

static void set_volume(int channel, float volume)
{
	float mix[12] = {0};
	mix[0] = volume;
	mix[1] = volume;
	ndspChnSetMix(channel, mix);
}

void audio_init(void)
{
	ready = R_SUCCEEDED(ndspInit());
	if (ready) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	}
}

int audio_available(void)
{
	return ready;
}

static void close_music(void)
{
	if (!music.active) {
		return;
	}
	ndspChnReset(MUSIC_CHANNEL);
	if (music.file != NULL) {
		fclose(music.file);
	}
	linearFree(music.memory);
	memset(&music, 0, sizeof(music));
}

static void sfx_release(Sfx *slot)
{
	if (slot->memory != NULL) {
		linearFree(slot->memory);
	}
	memset(slot, 0, sizeof(*slot));
}

void audio_stop_sfx(void)
{
	if (!ready) {
		return;
	}
	for (int index = 0; index < SFX_SLOTS; ++index) {
		ndspChnReset(SFX_FIRST + index);
		sfx_release(&sfx[index]);
	}
	next_sfx = 0;
}

void audio_exit(void)
{
	if (!ready) {
		return;
	}
	close_music();
	audio_stop_sfx();
	ndspExit();
	ready = 0;
}

static void say(char *err, int size, const char *text)
{
	if (err != NULL && size > 0) {
		snprintf(err, (size_t)size, "%s", text);
	}
}

/* Fills any finished buffers, wrapping to the start of the data to loop. */
static void feed_music(void)
{
	for (int index = 0; index < STREAM_BUFFERS; ++index) {
		ndspWaveBuf *buffer = &music.buffers[index];
		if (buffer->status != NDSP_WBUF_DONE && buffer->status != NDSP_WBUF_FREE) {
			continue;
		}
		u8 *target = (u8 *)music.memory + (size_t)index * STREAM_BYTES;
		size_t filled = 0;
		size_t want = STREAM_BYTES - STREAM_BYTES % (size_t)music.info.frame_size;
		while (filled < want) {
			long left = music.info.data_size - music.position;
			if (left <= 0) {
				music.position = 0;
				left = music.info.data_size;
			}
			size_t chunk = want - filled < (size_t)left ? want - filled : (size_t)left;
			fseek(music.file, music.info.data_offset + music.position, SEEK_SET);
			size_t got = fread(target + filled, 1, chunk, music.file);
			if (got == 0) {
				music.position = 0;
				break;
			}
			filled += got;
			music.position += (long)got;
		}
		if (filled == 0) {
			continue;
		}
		if (music.info.bits == 8) {
			for (size_t byte = 0; byte < filled; ++byte) {
				target[byte] = (u8)(target[byte] - 128);
			}
		}
		memset(buffer, 0, sizeof(*buffer));
		buffer->data_vaddr = target;
		buffer->nsamples = (u32)(filled / (size_t)music.info.frame_size);
		DSP_FlushDataCache(target, filled);
		ndspChnWaveBufAdd(MUSIC_CHANNEL, buffer);
	}
}

int audio_play_music(const char *path, float fade_seconds, char *err, int err_size)
{
	if (!ready) {
		say(err, err_size, "no audio (dspfirm.cdc missing?)");
		return 0;
	}
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		say(err, err_size, "audio file not found");
		return 0;
	}
	WavInfo info;
	char reason[64];
	if (!wav_open_info(file, &info, reason, sizeof(reason))) {
		fclose(file);
		say(err, err_size, reason);
		return 0;
	}
	void *memory = linearAlloc((size_t)STREAM_BUFFERS * STREAM_BYTES);
	if (memory == NULL) {
		fclose(file);
		say(err, err_size, "out of audio memory");
		return 0;
	}

	close_music();
	music.active = 1;
	music.file = file;
	music.info = info;
	music.memory = memory;
	music.target = 1.0f;
	music.volume = fade_seconds > 0.0f ? 0.0f : 1.0f;
	music.rate = fade_seconds > 0.0f ? 1.0f / fade_seconds : 0.0f;

	ndspChnReset(MUSIC_CHANNEL);
	ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(MUSIC_CHANNEL, (float)info.rate);
	ndspChnSetFormat(MUSIC_CHANNEL, ndsp_format(&info));
	set_volume(MUSIC_CHANNEL, music.volume);
	feed_music();
	return 1;
}

void audio_stop_music(float fade_seconds)
{
	if (!music.active) {
		return;
	}
	if (fade_seconds <= 0.0f) {
		close_music();
		return;
	}
	music.target = 0.0f;
	music.stopping = 1;
	music.rate = music.volume > 0.0f ? music.volume / fade_seconds : 1.0f;
}

int audio_play_sfx(const char *path, char *err, int err_size)
{
	if (!ready) {
		say(err, err_size, "no audio (dspfirm.cdc missing?)");
		return 0;
	}
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		say(err, err_size, "audio file not found");
		return 0;
	}
	WavInfo info;
	char reason[64];
	if (!wav_open_info(file, &info, reason, sizeof(reason))) {
		fclose(file);
		say(err, err_size, reason);
		return 0;
	}
	if (info.data_size > MAX_SFX_BYTES) {
		fclose(file);
		say(err, err_size, "sound effect too long (2 MB max)");
		return 0;
	}
	u8 *memory = linearAlloc((size_t)info.data_size);
	if (memory == NULL) {
		fclose(file);
		say(err, err_size, "out of audio memory");
		return 0;
	}
	fseek(file, info.data_offset, SEEK_SET);
	size_t got = fread(memory, 1, (size_t)info.data_size, file);
	fclose(file);
	if (got != (size_t)info.data_size) {
		linearFree(memory);
		say(err, err_size, "truncated audio file");
		return 0;
	}
	if (info.bits == 8) {
		for (size_t byte = 0; byte < got; ++byte) {
			memory[byte] = (u8)(memory[byte] - 128);
		}
	}

	int channel = SFX_FIRST + next_sfx;
	Sfx *slot = &sfx[next_sfx];
	next_sfx = (next_sfx + 1) % SFX_SLOTS;
	ndspChnReset(channel);
	sfx_release(slot);
	slot->memory = memory;
	slot->buffer.data_vaddr = memory;
	slot->buffer.nsamples = (u32)(info.data_size / info.frame_size);
	DSP_FlushDataCache(memory, (size_t)info.data_size);
	ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
	ndspChnSetRate(channel, (float)info.rate);
	ndspChnSetFormat(channel, ndsp_format(&info));
	set_volume(channel, 1.0f);
	ndspChnWaveBufAdd(channel, &slot->buffer);
	return 1;
}

void audio_update(float dt)
{
	if (!ready || !music.active) {
		return;
	}
	if (music.volume != music.target) {
		float step = music.rate * dt;
		if (music.volume < music.target) {
			music.volume = music.volume + step > music.target ? music.target : music.volume + step;
		} else {
			music.volume = music.volume - step < music.target ? music.target : music.volume - step;
		}
		set_volume(MUSIC_CHANNEL, music.volume);
	}
	if (music.stopping && music.volume <= 0.0f) {
		close_music();
		return;
	}
	feed_music();
}
