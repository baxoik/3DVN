#ifndef NVL_PAGE_H
#define NVL_PAGE_H

#include <stddef.h>
#include <stdint.h>

#define NVL_PAGE_MAX_BYTES 8192u
#define NVL_PAGE_EVENT_BITMAP_BYTES ((NVL_PAGE_MAX_BYTES + 1u + 7u) / 8u)
#define NVL_PAGE_SAVE_MAX_BYTES (12u + NVL_PAGE_MAX_BYTES + NVL_PAGE_EVENT_BITMAP_BYTES + 4u)

typedef struct {
	char text[NVL_PAGE_MAX_BYTES + 1u];
	uint16_t length;
	uint16_t current_event_offset;
	uint8_t event_starts[NVL_PAGE_EVENT_BITMAP_BYTES];
} NvlPage;

typedef enum {
	NVL_PAGE_OK,
	NVL_PAGE_TRIMMED,
	NVL_PAGE_INVALID_ARGUMENT,
	NVL_PAGE_INVALID_UTF8,
	NVL_PAGE_EMBEDDED_NUL,
	NVL_PAGE_CURRENT_EVENT_TOO_LARGE,
	NVL_PAGE_BUFFER_TOO_SMALL,
	NVL_PAGE_INVALID_STATE,
	NVL_PAGE_MALFORMED_DATA,
	NVL_PAGE_UNSUPPORTED_VERSION
} NvlPageResult;

/* Initialize an empty page. */
void nvl_page_clear(NvlPage *page);

/*
 * Append one resolved dialogue event. speaker_prefix is copied exactly before
 * resolved_text when its size is nonzero; no punctuation, spacing, or newline
 * is inserted. NULL is allowed for either input only when its size is zero.
 * Embedded NUL and invalid UTF-8 are rejected. The event is atomic: on failure
 * the page is unchanged. Earlier complete events are discarded as needed to
 * fit the 8192-byte limit. Returns NVL_PAGE_TRIMMED when one or more previous
 * events were discarded.
 */
NvlPageResult nvl_page_append_dialogue(
	NvlPage *page,
	const char *speaker_prefix,
	size_t speaker_prefix_size,
	const char *resolved_text,
	size_t resolved_text_size);

/* Return the exact versioned save-extension size for a valid page. */
NvlPageResult nvl_page_encoded_size(const NvlPage *page, size_t *encoded_size);

/*
 * Encode the page into the tagged, versioned save extension. encoded_size is
 * set to the required size on success or when output_capacity is too small.
 */
NvlPageResult nvl_page_encode(
	const NvlPage *page,
	uint8_t *output,
	size_t output_capacity,
	size_t *encoded_size);

/* Decode exactly one save extension; trailing bytes and invalid state fail. */
NvlPageResult nvl_page_decode(const uint8_t *data, size_t data_size, NvlPage *page);

#endif
