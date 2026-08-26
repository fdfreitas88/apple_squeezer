#include "../pcm_convert.h"

#include <assert.h>
#include <stdio.h>

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
	puts("PCM endian and signedness conversion tests passed");
	return 0;
}
