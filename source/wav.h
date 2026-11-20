#ifndef WAV_H
#define WAV_H

#include <stdio.h>

typedef struct {
	int channels;         /* 1 or 2 */
	int bits;             /* 8 or 16 */
	long rate;            /* sample rate in Hz */
	long data_offset;     /* file offset of the first sample */
	long data_size;       /* bytes of sample data, whole frames only */
	int frame_size;       /* bytes per frame (decoded) */
	/* MS-ADPCM (format 2): data_size is the compressed size, bits is 16 (decoded) */
	int adpcm;
	int block_align;      /* bytes per compressed block */
	int samples_per_block;
	int coef_count;
	short coef[32][2];
	long frames;          /* decoded frames in the whole file */
} WavInfo;

/* Reads a PCM or MS-ADPCM WAV header. Returns 1 on success, else 0 with a message in err. */
int wav_open_info(FILE *file, WavInfo *info, char *err, size_t err_size);

/* Decodes one MS-ADPCM block (`size` bytes; a short final block is allowed) into
   interleaved 16-bit samples. Returns the frames written (at most samples_per_block). */
int wav_adpcm_block(const WavInfo *info, const unsigned char *block, size_t size, short *out);

#endif
