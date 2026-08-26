#ifndef APPLE_SQUEEZER_PCM_CONVERT_H
#define APPLE_SQUEEZER_PCM_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

static inline uint32_t pcm_read_s32(const uint8_t *p, unsigned sample_size,
		bool bigendian, bool unsigned_8bit) {
	uint32_t raw;
	if (sample_size == 1) {
		int value = unsigned_8bit ? (int)p[0] - 128 : (int)(int8_t)p[0];
		return (uint32_t)(int32_t)value << 24;
	}
	if (sample_size == 2) {
		raw = bigendian ? ((uint32_t)p[0] << 8) | p[1] :
			(uint32_t)p[0] | ((uint32_t)p[1] << 8);
		return (uint32_t)(int32_t)(int16_t)raw << 16;
	}
	if (sample_size == 3) {
		raw = bigendian ? ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2] :
			(uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
		if (raw & UINT32_C(0x00800000)) raw |= UINT32_C(0xff000000);
		return raw << 8;
	}
	return bigendian ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3] : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
