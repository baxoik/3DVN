#ifndef VID_H
#define VID_H

#include <stddef.h>

/*
 * 3VV video container (little-endian):
 *   0   "3VV1"
 *   4   u16 width   (1..512)
 *   6   u16 height  (1..256)
 *   8   u32 fps x1000 (1000..60000)
 *   12  u32 frame count (1..VID_MAX_FRAMES)
 *   16  u32 offsets[frames + 1]   absolute file offsets; frame i occupies
 *                                  [offsets[i], offsets[i+1])
 *   ...  baseline/progressive JPEG frames, each independently decodable
 */
#define VID_HEADER_SIZE 16
#define VID_MAX_WIDTH 512
#define VID_MAX_HEIGHT 256
#define VID_MAX_FRAMES 100000
#define VID_MAX_FRAME_BYTES (256 * 1024)

typedef struct {
	unsigned width, height, fps_milli, frames;
} VidHeader;

/* Returns 1 if the first VID_HEADER_SIZE bytes are a valid header. */
int vid_parse_header(const unsigned char *data, size_t size, VidHeader *header);

/* Reads the frame count + 1 offsets that follow the header; 1 if they are sane for a file of file_size bytes. */
int vid_read_index(const unsigned char *data, size_t size, const VidHeader *header,
	size_t file_size, unsigned *offsets);

/* Frame to show after the given playback time, clamped to the last frame. */
unsigned vid_frame_at(const VidHeader *header, float seconds);

/* True once the playback time is past the last frame. */
int vid_finished(const VidHeader *header, float seconds);

#endif
