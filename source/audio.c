#include "audio.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pack.h"
#include "wav.h"

/* Ogg Vorbis: music and voices can be .ogg (much smaller than .wav); decoded while
   streaming with stb_vorbis (public domain, pull API on a FILE*). */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#include "stb_vorbis.h"

#define MUSIC_CHANNEL 0
#define SFX_FIRST 1
#define SFX_SLOTS 4
#define STREAM_BUFFERS 4
#define STREAM_BYTES (32 * 1024)
#define MAX_SFX_BYTES (4 * 1024 * 1024)   /* decoded size */
#define SFX_STREAM_BUFFERS 2
#define SFX_STREAM_BYTES (16 * 1024)

typedef struct {
	int active;
	FILE *file;
	stb_vorbis *vorbis;   /* set for .ogg music (then file is owned by the decoder) */
	WavInfo info;
	void *memory;
	ndspWaveBuf buffers[STREAM_BUFFERS];
	long position;
	unsigned char *block;          /* ADPCM: one compressed block ... */
	short *decoded;                /* ... and its decoded frames */
	int decoded_frames, decoded_used;
	float volume, target, rate;
	int stopping;
} Music;

typedef struct {
	void *memory;
	ndspWaveBuf buffer;            /* WAV: the whole sound in one buffer */
	stb_vorbis *vorbis;            /* Ogg: streamed through two small buffers */
	ndspWaveBuf buffers[SFX_STREAM_BUFFERS];
	int channels, finished;
	int voice;                     /* from audio/v/: follows the voice volume */
} Sfx;

static int ready;
static float master_gain = 1.0f, music_gain = 1.0f, voice_gain = 1.0f;
static Music music;
static Sfx sfx[SFX_SLOTS];
static int next_sfx;

/* WAV PCM data is little-endian; the mixer reads native samples (big-endian on Wii U). */
static void pcm16_to_native(void *data, size_t bytes)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	unsigned char *p = data;
	for (size_t i = 0; i + 1 < bytes; i += 2) {
		unsigned char t = p[i];
		p[i] = p[i + 1];
		p[i + 1] = t;
	}
#else
	(void)data;
	(void)bytes;
#endif
}

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

static float music_level(void)
{
	return music.volume * master_gain * music_gain;
}

static float sfx_level(int voice)
{
	return master_gain * (voice ? voice_gain : 1.0f);
}

void audio_set_volumes(float master, float music_volume, float voice)
{
	master_gain = master;
	music_gain = music_volume;
	voice_gain = voice;
	if (!ready) return;
	if (music.active) set_volume(MUSIC_CHANNEL, music_level());
	for (int index = 0; index < SFX_SLOTS; ++index) {
		set_volume(SFX_FIRST + index, sfx_level(sfx[index].voice));
	}
}

static int is_voice_path(const char *path)
{
	return strstr(path, "/audio/v/") != NULL || strncmp(path, "audio/v/", 8) == 0;
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
	if (music.vorbis != NULL) {
		stb_vorbis_close(music.vorbis);
	} else if (music.file != NULL) {
		fclose(music.file);
	}
	linearFree(music.memory);
	free(music.block);
	free(music.decoded);
	memset(&music, 0, sizeof(music));
}

static void sfx_release(Sfx *slot)
{
	if (slot->vorbis != NULL) {
		stb_vorbis_close(slot->vorbis);
	}
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

int audio_sfx_playing(void)
{
	if (!ready) {
		return 0;
	}
	for (int index = 0; index < SFX_SLOTS; ++index) {
		/* The buffer is QUEUED as soon as it is added, before the DSP reports the channel as playing. */
		const Sfx *slot = &sfx[index];
		if (slot->memory == NULL) continue;
		if (slot->vorbis != NULL) {
			if (!slot->finished) return 1;
			for (int b = 0; b < SFX_STREAM_BUFFERS; ++b) {
				u8 status = slot->buffers[b].status;
				if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING) return 1;
			}
			continue;
		}
		u8 status = slot->buffer.status;
		if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING) {
			return 1;
		}
	}
	return 0;
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
		while (music.vorbis != NULL && filled < want) {
			int channels = music.info.channels;
			int frames = stb_vorbis_get_samples_short_interleaved(music.vorbis, channels,
				(short *)(target + filled), (int)((want - filled) / 2));
			if (frames <= 0) {
				if (filled == 0 && music.position == -1) break;   /* empty file: give up */
				stb_vorbis_seek_start(music.vorbis);              /* loop */
				music.position = -1;
				continue;
			}
			music.position = 0;
			filled += (size_t)frames * (size_t)music.info.frame_size;
		}
		int empty_blocks = 0;   /* guards against a file with no decodable block */
		while (music.vorbis == NULL && music.info.adpcm && filled < want && empty_blocks < 4) {
			if (music.decoded_used >= music.decoded_frames) {
				if (music.position >= music.info.data_size) music.position = 0;   /* loop */
				long left = music.info.data_size - music.position;
				size_t size = left < music.info.block_align ? (size_t)left : (size_t)music.info.block_align;
				fseek(music.file, music.info.data_offset + music.position, SEEK_SET);
				size_t got = fread(music.block, 1, size, music.file);
				music.position += (long)size;
				music.decoded_frames = wav_adpcm_block(&music.info, music.block, got, music.decoded);
				music.decoded_used = 0;
				if (music.decoded_frames <= 0) {
					++empty_blocks;
					continue;
				}
				empty_blocks = 0;
			}
			size_t frames = (want - filled) / (size_t)music.info.frame_size;
			size_t have = (size_t)(music.decoded_frames - music.decoded_used);
			if (frames > have) frames = have;
			memcpy(target + filled, music.decoded + (size_t)music.decoded_used * music.info.channels,
				frames * (size_t)music.info.frame_size);
			music.decoded_used += (int)frames;
			filled += frames * (size_t)music.info.frame_size;
		}
		while (music.vorbis == NULL && !music.info.adpcm && filled < want) {
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
		} else if (music.vorbis == NULL && !music.info.adpcm) {
			pcm16_to_native(target, filled);
		}
		memset(buffer, 0, sizeof(*buffer));
		buffer->data_vaddr = target;
		buffer->nsamples = (u32)(filled / (size_t)music.info.frame_size);
		DSP_FlushDataCache(target, filled);
		ndspChnWaveBufAdd(MUSIC_CHANNEL, buffer);
	}
}

/* If `file` is Ogg Vorbis, opens a decoder (which then owns the file) and fills `info` as
   16-bit PCM. Returns 1 for Ogg, 0 for not Ogg (file rewound), -1 on a broken Ogg file. */
static int open_ogg(FILE *file, stb_vorbis **out, WavInfo *info, char *err, int err_size)
{
	char magic[4];
	*out = NULL;
	if (fread(magic, 1, 4, file) != 4 || memcmp(magic, "OggS", 4) != 0) {
		fseek(file, 0, SEEK_SET);
		return 0;
	}
	fseek(file, 0, SEEK_SET);
	int error = 0;
	stb_vorbis *vorbis = stb_vorbis_open_file(file, 1, &error, NULL);
	if (vorbis == NULL) {
		fclose(file);
		say(err, err_size, "bad Ogg Vorbis file");
		return -1;
	}
	stb_vorbis_info v = stb_vorbis_get_info(vorbis);
	memset(info, 0, sizeof(*info));
	info->channels = v.channels > 2 ? 2 : v.channels;
	info->bits = 16;
	info->rate = (int)v.sample_rate;
	info->frame_size = 2 * info->channels;
	*out = vorbis;
	return 1;
}

int audio_play_music(const char *path, float fade_seconds, char *err, int err_size)
{
	if (!ready) {
		say(err, err_size, "no audio (dspfirm.cdc missing?)");
		return 0;
	}
	FILE *file = vn_fopen(path, "rb");
	if (file == NULL) {
		say(err, err_size, "audio file not found");
		return 0;
	}
	WavInfo info;
	stb_vorbis *vorbis = NULL;
	if (open_ogg(file, &vorbis, &info, err, err_size) < 0) {
		return 0;
	}
	char reason[64];
	if (vorbis == NULL && !wav_open_info(file, &info, reason, sizeof(reason))) {
		fclose(file);
		say(err, err_size, reason);
		return 0;
	}
	void *memory = linearAlloc((size_t)STREAM_BUFFERS * STREAM_BYTES);
	unsigned char *block = NULL;
	short *decoded = NULL;
	if (memory != NULL && vorbis == NULL && info.adpcm) {
		block = malloc((size_t)info.block_align);
		decoded = malloc((size_t)info.samples_per_block * (size_t)info.frame_size);
	}
	if (memory == NULL || (vorbis == NULL && info.adpcm && (block == NULL || decoded == NULL))) {
		if (memory != NULL) linearFree(memory);
		free(block);
		free(decoded);
		if (vorbis != NULL) stb_vorbis_close(vorbis); else fclose(file);
		say(err, err_size, "out of audio memory");
		return 0;
	}

	close_music();
	music.block = block;
	music.decoded = decoded;
	music.decoded_frames = music.decoded_used = 0;
	music.active = 1;
	music.file = file;
	music.vorbis = vorbis;
	music.info = info;
	music.memory = memory;
	music.target = 1.0f;
	music.volume = fade_seconds > 0.0f ? 0.0f : 1.0f;
	music.rate = fade_seconds > 0.0f ? 1.0f / fade_seconds : 0.0f;

	ndspChnReset(MUSIC_CHANNEL);
	ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(MUSIC_CHANNEL, (float)info.rate);
	ndspChnSetFormat(MUSIC_CHANNEL, ndsp_format(&info));
	set_volume(MUSIC_CHANNEL, music_level());
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

/* Refills an Ogg sound effect's finished buffers; marks it finished at the end of the file. */
static void feed_sfx(Sfx *slot, int channel)
{
	for (int index = 0; index < SFX_STREAM_BUFFERS && !slot->finished; ++index) {
		ndspWaveBuf *buffer = &slot->buffers[index];
		if (buffer->status != NDSP_WBUF_DONE && buffer->status != NDSP_WBUF_FREE) {
			continue;
		}
		short *target = (short *)((u8 *)slot->memory + (size_t)index * SFX_STREAM_BYTES);
		int frames = stb_vorbis_get_samples_short_interleaved(slot->vorbis, slot->channels, target,
			SFX_STREAM_BYTES / 2);
		if (frames <= 0) {
			slot->finished = 1;
			break;
		}
		memset(buffer, 0, sizeof(*buffer));
		buffer->data_vaddr = target;
		buffer->nsamples = (u32)frames;
		DSP_FlushDataCache(target, (u32)frames * 2u * (u32)slot->channels);
		ndspChnWaveBufAdd(channel, buffer);
	}
}

int audio_play_sfx(const char *path, char *err, int err_size)
{
	if (!ready) {
		say(err, err_size, "no audio (dspfirm.cdc missing?)");
		return 0;
	}
	FILE *file = vn_fopen(path, "rb");
	if (file == NULL) {
		say(err, err_size, "audio file not found");
		return 0;
	}
	WavInfo info;
	stb_vorbis *vorbis = NULL;
	int ogg = open_ogg(file, &vorbis, &info, err, err_size);
	if (ogg < 0) {
		return 0;
	}
	if (ogg) {
		void *buffers = linearAlloc((size_t)SFX_STREAM_BUFFERS * SFX_STREAM_BYTES);
		if (buffers == NULL) {
			stb_vorbis_close(vorbis);
			say(err, err_size, "out of audio memory");
			return 0;
		}
		int channel = SFX_FIRST + next_sfx;
		Sfx *slot = &sfx[next_sfx];
		next_sfx = (next_sfx + 1) % SFX_SLOTS;
		ndspChnReset(channel);
		sfx_release(slot);
		slot->memory = buffers;
		slot->vorbis = vorbis;
		slot->channels = info.channels;
		slot->voice = is_voice_path(path);
		ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
		ndspChnSetRate(channel, (float)info.rate);
		ndspChnSetFormat(channel, ndsp_format(&info));
		set_volume(channel, sfx_level(slot->voice));
		feed_sfx(slot, channel);
		return 1;
	}
	char reason[64];
	if (!wav_open_info(file, &info, reason, sizeof(reason))) {
		fclose(file);
		say(err, err_size, reason);
		return 0;
	}
	long bytes = info.frames * info.frame_size;   /* decoded size */
	if (bytes > MAX_SFX_BYTES) {
		fclose(file);
		say(err, err_size, "sound effect too long (4 MB max)");
		return 0;
	}
	u8 *memory = linearAlloc((size_t)bytes);
	u8 *packed = info.adpcm ? malloc((size_t)info.data_size) : memory;
	if (memory == NULL || packed == NULL) {
		if (memory != NULL) linearFree(memory);
		fclose(file);
		say(err, err_size, "out of audio memory");
		return 0;
	}
	fseek(file, info.data_offset, SEEK_SET);
	size_t got = fread(packed, 1, (size_t)info.data_size, file);
	fclose(file);
	if (got != (size_t)info.data_size) {
		if (packed != memory) free(packed);
		linearFree(memory);
		say(err, err_size, "truncated audio file");
		return 0;
	}
	if (info.adpcm) {
		long frames = 0;
		for (long offset = 0; offset < info.data_size && frames < info.frames; offset += info.block_align) {
			long size = info.data_size - offset < info.block_align ? info.data_size - offset : info.block_align;
			frames += wav_adpcm_block(&info, packed + offset, (size_t)size,
				(short *)memory + (size_t)frames * (size_t)info.channels);
		}
		free(packed);
		info.data_size = frames * info.frame_size;
		got = (size_t)info.data_size;
	}
	if (info.bits == 8) {
		for (size_t byte = 0; byte < got; ++byte) {
			memory[byte] = (u8)(memory[byte] - 128);
		}
	} else if (!info.adpcm) {
		pcm16_to_native(memory, got);
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
	slot->voice = is_voice_path(path);
	ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
	ndspChnSetRate(channel, (float)info.rate);
	ndspChnSetFormat(channel, ndsp_format(&info));
	set_volume(channel, sfx_level(slot->voice));
	ndspChnWaveBufAdd(channel, &slot->buffer);
	return 1;
}

void audio_update(float dt)
{
	if (!ready) {
		return;
	}
	for (int index = 0; index < SFX_SLOTS; ++index) {
		if (sfx[index].vorbis != NULL && !sfx[index].finished) {
			feed_sfx(&sfx[index], SFX_FIRST + index);
		}
	}
	if (!music.active) {
		return;
	}
	if (music.volume != music.target) {
		float step = music.rate * dt;
		if (music.volume < music.target) {
			music.volume = music.volume + step > music.target ? music.target : music.volume + step;
		} else {
			music.volume = music.volume - step < music.target ? music.target : music.volume - step;
		}
		set_volume(MUSIC_CHANNEL, music_level());
	}
	if (music.stopping && music.volume <= 0.0f) {
		close_music();
		return;
	}
	feed_music();
}
