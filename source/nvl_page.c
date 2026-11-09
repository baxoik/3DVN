#include "nvl_page.h"

#include <stdbool.h>
#include <string.h>

#define NVL_SAVE_HEADER_BYTES 12u
#define NVL_SAVE_CHECKSUM_BYTES 4u
#define NVL_SAVE_VERSION 1u

static const uint8_t nvl_save_tag[4] = { 'N', 'V', 'P', 'G' };

static size_t bitmap_size(size_t text_length)
{
	return (text_length + 1u + 7u) / 8u;
}

static bool bit_is_set(const uint8_t *bitmap, size_t offset)
{
	return (bitmap[offset / 8u] & (uint8_t)(1u << (offset % 8u))) != 0;
}

static void set_bit(uint8_t *bitmap, size_t offset)
{
	bitmap[offset / 8u] |= (uint8_t)(1u << (offset % 8u));
}

static bool valid_utf8(const uint8_t *text, size_t length)
{
	size_t index = 0;

	while (index < length) {
		uint8_t first = text[index++];
		uint32_t codepoint;
		size_t continuation_count;

		if (first <= 0x7fu)
			continue;
		if (first >= 0xc2u && first <= 0xdfu) {
			codepoint = first & 0x1fu;
			continuation_count = 1u;
		} else if (first >= 0xe0u && first <= 0xefu) {
			codepoint = first & 0x0fu;
			continuation_count = 2u;
		} else if (first >= 0xf0u && first <= 0xf4u) {
			codepoint = first & 0x07u;
			continuation_count = 3u;
		} else {
			return false;
		}

		if (continuation_count > length - index)
			return false;
		for (size_t count = 0; count < continuation_count; ++count) {
			uint8_t next = text[index++];
			if ((next & 0xc0u) != 0x80u)
				return false;
			codepoint = (codepoint << 6) | (next & 0x3fu);
		}
		if ((continuation_count == 1u && codepoint < 0x80u)
			|| (continuation_count == 2u && codepoint < 0x800u)
			|| (continuation_count == 3u && codepoint < 0x10000u)
			|| (codepoint >= 0xd800u && codepoint <= 0xdfffu)
			|| codepoint > 0x10ffffu)
			return false;
	}
	return true;
}

static bool utf8_boundary(const uint8_t *text, size_t length, size_t offset)
{
	return offset == 0u || offset == length || (text[offset] & 0xc0u) != 0x80u;
}

static bool valid_page(const NvlPage *page)
{
	size_t length, current;
	bool has_events = false;

	if (page == NULL)
		return false;
	length = page->length;
	current = page->current_event_offset;
	if (length > NVL_PAGE_MAX_BYTES || current > length
		|| page->text[length] != '\0'
		|| memchr(page->text, '\0', length) != NULL
		|| !valid_utf8((const uint8_t *)page->text, length)
		|| !utf8_boundary((const uint8_t *)page->text, length, current))
		return false;

	for (size_t offset = 0; offset < sizeof(page->event_starts) * 8u; ++offset) {
		bool set = bit_is_set(page->event_starts, offset);
		if (offset > length && set)
			return false;
		if (set) {
			has_events = true;
			if (offset > current
				|| !utf8_boundary((const uint8_t *)page->text, length, offset))
				return false;
		}
	}
	if (!has_events)
		return length == 0u && current == 0u;
	return bit_is_set(page->event_starts, 0u)
		&& bit_is_set(page->event_starts, current);
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffu;

	for (size_t index = 0; index < length; ++index) {
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8u; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
	}
	return ~crc;
}

static void put_u16le(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16le(const uint8_t *input)
{
	return (uint16_t)((uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8));
}

static void put_u32le(uint8_t *output, uint32_t value)
{
	for (unsigned index = 0; index < 4u; ++index)
		output[index] = (uint8_t)(value >> (index * 8u));
}

static uint32_t get_u32le(const uint8_t *input)
{
	return (uint32_t)input[0]
		| ((uint32_t)input[1] << 8)
		| ((uint32_t)input[2] << 16)
		| ((uint32_t)input[3] << 24);
}

void nvl_page_clear(NvlPage *page)
{
	if (page != NULL)
		memset(page, 0, sizeof(*page));
}

NvlPageResult nvl_page_append_dialogue(
	NvlPage *page,
	const char *speaker_prefix,
	size_t speaker_prefix_size,
	const char *resolved_text,
	size_t resolved_text_size)
{
	uint8_t chunk[NVL_PAGE_MAX_BYTES];
	uint8_t shifted_starts[NVL_PAGE_EVENT_BITMAP_BYTES] = { 0 };
	size_t chunk_size, old_length, trim = 0, new_length;

	if (page == NULL || (speaker_prefix == NULL && speaker_prefix_size != 0u)
		|| (resolved_text == NULL && resolved_text_size != 0u))
		return NVL_PAGE_INVALID_ARGUMENT;
	if (!valid_page(page))
		return NVL_PAGE_INVALID_STATE;
	if (speaker_prefix_size > NVL_PAGE_MAX_BYTES
		|| resolved_text_size > NVL_PAGE_MAX_BYTES - speaker_prefix_size)
		return NVL_PAGE_CURRENT_EVENT_TOO_LARGE;
	chunk_size = speaker_prefix_size + resolved_text_size;
	if ((speaker_prefix_size != 0u && memchr(speaker_prefix, '\0', speaker_prefix_size) != NULL)
		|| (resolved_text_size != 0u && memchr(resolved_text, '\0', resolved_text_size) != NULL))
		return NVL_PAGE_EMBEDDED_NUL;
	if ((speaker_prefix_size != 0u
			&& !valid_utf8((const uint8_t *)speaker_prefix, speaker_prefix_size))
		|| (resolved_text_size != 0u
			&& !valid_utf8((const uint8_t *)resolved_text, resolved_text_size)))
		return NVL_PAGE_INVALID_UTF8;
	if (speaker_prefix_size != 0u)
		memcpy(chunk, speaker_prefix, speaker_prefix_size);
	if (resolved_text_size != 0u)
		memcpy(chunk + speaker_prefix_size, resolved_text, resolved_text_size);

	old_length = page->length;
	if (old_length + chunk_size > NVL_PAGE_MAX_BYTES) {
		for (size_t candidate = 1u; candidate <= old_length; ++candidate) {
			if (bit_is_set(page->event_starts, candidate)
				&& old_length - candidate + chunk_size <= NVL_PAGE_MAX_BYTES) {
				trim = candidate;
				break;
			}
		}
		if (old_length - trim + chunk_size > NVL_PAGE_MAX_BYTES)
			trim = old_length;
	}
	new_length = old_length - trim + chunk_size;

	for (size_t offset = trim; offset <= old_length; ++offset) {
		if (bit_is_set(page->event_starts, offset))
			set_bit(shifted_starts, offset - trim);
	}
	set_bit(shifted_starts, old_length - trim);

	if (trim != 0u)
		memmove(page->text, page->text + trim, old_length - trim);
	if (chunk_size != 0u)
		memcpy(page->text + old_length - trim, chunk, chunk_size);
	page->text[new_length] = '\0';
	page->length = (uint16_t)new_length;
	page->current_event_offset = (uint16_t)(old_length - trim);
	memcpy(page->event_starts, shifted_starts, sizeof(page->event_starts));
	return trim != 0u ? NVL_PAGE_TRIMMED : NVL_PAGE_OK;
}

NvlPageResult nvl_page_encoded_size(const NvlPage *page, size_t *encoded_size)
{
	if (page == NULL || encoded_size == NULL)
		return NVL_PAGE_INVALID_ARGUMENT;
	if (!valid_page(page))
		return NVL_PAGE_INVALID_STATE;
	*encoded_size = NVL_SAVE_HEADER_BYTES + page->length
		+ bitmap_size(page->length) + NVL_SAVE_CHECKSUM_BYTES;
	return NVL_PAGE_OK;
}

NvlPageResult nvl_page_encode(
	const NvlPage *page,
	uint8_t *output,
	size_t output_capacity,
	size_t *encoded_size)
{
	size_t required, bits_size;
	NvlPageResult result;

	if (output == NULL || encoded_size == NULL)
		return NVL_PAGE_INVALID_ARGUMENT;
	result = nvl_page_encoded_size(page, &required);
	if (result != NVL_PAGE_OK)
		return result;
	*encoded_size = required;
	if (output_capacity < required)
		return NVL_PAGE_BUFFER_TOO_SMALL;

	bits_size = bitmap_size(page->length);
	memcpy(output, nvl_save_tag, sizeof(nvl_save_tag));
	put_u16le(output + 4u, NVL_SAVE_VERSION);
	put_u16le(output + 6u, page->length);
	put_u16le(output + 8u, page->current_event_offset);
	put_u16le(output + 10u, (uint16_t)bits_size);
	memcpy(output + NVL_SAVE_HEADER_BYTES, page->text, page->length);
	memcpy(output + NVL_SAVE_HEADER_BYTES + page->length,
		page->event_starts, bits_size);
	put_u32le(output + required - NVL_SAVE_CHECKSUM_BYTES,
		crc32(output, required - NVL_SAVE_CHECKSUM_BYTES));
	return NVL_PAGE_OK;
}

NvlPageResult nvl_page_decode(const uint8_t *data, size_t data_size, NvlPage *page)
{
	NvlPage parsed;
	size_t length, current, bits_size, expected_size;

	if (data == NULL || page == NULL)
		return NVL_PAGE_INVALID_ARGUMENT;
	if (data_size < NVL_SAVE_HEADER_BYTES + NVL_SAVE_CHECKSUM_BYTES)
		return NVL_PAGE_MALFORMED_DATA;
	if (memcmp(data, nvl_save_tag, sizeof(nvl_save_tag)) != 0)
		return NVL_PAGE_MALFORMED_DATA;
	if (get_u16le(data + 4u) != NVL_SAVE_VERSION)
		return NVL_PAGE_UNSUPPORTED_VERSION;

	length = get_u16le(data + 6u);
	current = get_u16le(data + 8u);
	bits_size = get_u16le(data + 10u);
	if (length > NVL_PAGE_MAX_BYTES || current > length
		|| bits_size != bitmap_size(length))
		return NVL_PAGE_MALFORMED_DATA;
	expected_size = NVL_SAVE_HEADER_BYTES + length + bits_size + NVL_SAVE_CHECKSUM_BYTES;
	if (data_size != expected_size)
		return NVL_PAGE_MALFORMED_DATA;
	if (get_u32le(data + expected_size - NVL_SAVE_CHECKSUM_BYTES)
		!= crc32(data, expected_size - NVL_SAVE_CHECKSUM_BYTES))
		return NVL_PAGE_MALFORMED_DATA;

	nvl_page_clear(&parsed);
	parsed.length = (uint16_t)length;
	parsed.current_event_offset = (uint16_t)current;
	memcpy(parsed.text, data + NVL_SAVE_HEADER_BYTES, length);
	parsed.text[length] = '\0';
	memcpy(parsed.event_starts, data + NVL_SAVE_HEADER_BYTES + length, bits_size);
	if (memchr(parsed.text, '\0', length) != NULL)
		return NVL_PAGE_EMBEDDED_NUL;
	if (!valid_utf8((const uint8_t *)parsed.text, length))
		return NVL_PAGE_INVALID_UTF8;
	if (!valid_page(&parsed))
		return NVL_PAGE_MALFORMED_DATA;
	*page = parsed;
	return NVL_PAGE_OK;
}
