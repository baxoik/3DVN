#ifndef WAV_H
#define WAV_H

#include <stdio.h>

typedef struct {
	int channels;         /* 1 or 2 */
	int bits;             /* 8 or 16 */
	long rate;            /* sample rate in Hz */
	long data_offset;     /* file offset of the first sample */
	long data_size;       /* bytes of sample data, whole frames only */
	int frame_size;       /* bytes per frame */
} WavInfo;

/* Reads a PCM WAV header. Returns 1 on success, else 0 with a message in err. */
int wav_open_info(FILE *file, WavInfo *info, char *err, size_t err_size);

#endif
