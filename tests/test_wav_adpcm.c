/* Decodes a WAV (PCM or MS-ADPCM) with wav.c and writes raw 16-bit little-endian
   samples to stdout, so the result can be compared with a reference decoder:
     cc -Isource tests/test_wav_adpcm.c source/wav.c -o test_wav_adpcm
     ./test_wav_adpcm in.wav | cmp - <(ffmpeg -v error -i in.wav -f s16le -)  */
#include <stdio.h>
#include <stdlib.h>
#include "wav.h"

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s FILE.wav\n", argv[0]);
		return 2;
	}
	FILE *file = fopen(argv[1], "rb");
	WavInfo info;
	char err[64];
	if (file == NULL || !wav_open_info(file, &info, err, sizeof(err))) {
		fprintf(stderr, "%s: %s\n", argv[1], file == NULL ? "cannot open" : err);
		return 1;
	}
	unsigned char *data = malloc((size_t)info.data_size);
	fseek(file, info.data_offset, SEEK_SET);
	if (data == NULL || fread(data, 1, (size_t)info.data_size, file) != (size_t)info.data_size) return 1;
	if (!info.adpcm) {
		fwrite(data, 1, (size_t)info.data_size, stdout);
		return 0;
	}
	short *out = malloc((size_t)info.samples_per_block * (size_t)info.frame_size);
	long frames = 0;
	for (long offset = 0; offset < info.data_size; offset += info.block_align) {
		long size = info.data_size - offset < info.block_align ? info.data_size - offset : info.block_align;
		int got = wav_adpcm_block(&info, data + offset, (size_t)size, out);
		fwrite(out, (size_t)info.frame_size, (size_t)got, stdout);
		frames += got;
	}
	fprintf(stderr, "%ld frames (header says %ld)\n", frames, info.frames);
	return frames == info.frames ? 0 : 1;
}
