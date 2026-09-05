#include "../pcm_convert.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void check_endian_pair(unsigned bytes, uint32_t expected,
		const uint8_t *little, const uint8_t *big) {
	assert(pcm_read_s32(little, bytes, false, false) == expected);
	assert(pcm_read_s32(big, bytes, true, false) == expected);
}

int main(void) {
	const uint8_t signed8[] = { 0x80 };
	const uint8_t unsigned8[] = { 0x00 };
	const uint8_t le16[] = { 0x00, 0x80 };
	const uint8_t be16[] = { 0x80, 0x00 };
	const uint8_t le24[] = { 0x00, 0x00, 0x80 };
	const uint8_t be24[] = { 0x80, 0x00, 0x00 };
	const uint8_t le32[] = { 0x00, 0x00, 0x00, 0x80 };
	const uint8_t be32[] = { 0x80, 0x00, 0x00, 0x00 };
	assert(pcm_read_s32(signed8, 1, false, false) == UINT32_C(0x80000000));
	assert(pcm_read_s32(unsigned8, 1, false, true) == UINT32_C(0x80000000));
	assert(pcm_read_s32(le16, 2, false, false) == UINT32_C(0x80000000));
	assert(pcm_read_s32(be16, 2, true, false) == UINT32_C(0x80000000));
	assert(pcm_read_s32(le24, 3, false, false) == UINT32_C(0x80000000));
	assert(pcm_read_s32(be24, 3, true, false) == UINT32_C(0x80000000));
	assert(pcm_read_s32(le32, 4, false, false) == UINT32_C(0x80000000));
	assert(pcm_read_s32(be32, 4, true, false) == UINT32_C(0x80000000));

	/* Exercise non-symmetric values: tests containing only zero and the sign
	 * bit cannot detect a reversed byte order. */
	{
		const uint8_t le16_value[] = { 0x34, 0x12 };
		const uint8_t be16_value[] = { 0x12, 0x34 };
		const uint8_t le24_value[] = { 0x56, 0x34, 0x12 };
		const uint8_t be24_value[] = { 0x12, 0x34, 0x56 };
		const uint8_t le32_value[] = { 0x78, 0x56, 0x34, 0x12 };
		const uint8_t be32_value[] = { 0x12, 0x34, 0x56, 0x78 };
		const uint8_t le16_negative[] = { 0xcc, 0xed };
		const uint8_t be16_negative[] = { 0xed, 0xcc };
		const uint8_t le24_negative[] = { 0xaa, 0xcb, 0xed };
		const uint8_t be24_negative[] = { 0xed, 0xcb, 0xaa };
		check_endian_pair(2, UINT32_C(0x12340000), le16_value, be16_value);
		check_endian_pair(3, UINT32_C(0x12345600), le24_value, be24_value);
		check_endian_pair(4, UINT32_C(0x12345678), le32_value, be32_value);
		check_endian_pair(2, UINT32_C(0xedcc0000), le16_negative, be16_negative);
		check_endian_pair(3, UINT32_C(0xedcbaa00), le24_negative, be24_negative);
	}
	assert(pcm_read_s32(le32, 0, false, false) == 0);
	assert(pcm_read_s32(le32, 5, false, false) == 0);
	assert(pcm_s32_to_float(UINT32_C(0x00000000)) == 0.0f);
	assert(pcm_s32_to_float(UINT32_C(0x80000000)) == -1.0f);
	assert(fabsf(pcm_s32_to_float(UINT32_C(0x40000000)) - 0.5f) < 1e-7f);
	/* Float32 cannot distinguish INT32_MAX / 2^31 from 1.0f. */
	assert(pcm_s32_to_float(UINT32_C(0x7fffffff)) == 1.0f);
	puts("PCM endian and signedness conversion tests passed");
	return 0;
}
