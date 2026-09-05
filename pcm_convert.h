#ifndef APPLE_SQUEEZER_PCM_CONVERT_H
#define APPLE_SQUEEZER_PCM_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

static inline float pcm_s32_to_float(uint32_t sample) {
	return (float)(int32_t)sample * (1.0f / 2147483648.0f);
}

static inline uint32_t pcm_read_s32(const uint8_t *p, unsigned sample_size,
		bool bigendian, bool unsigned_8bit) {
	uint32_t raw;
	if (sample_size == 1) {
		/* Work entirely in the unsigned domain.  Left-shifting a negative
		 * signed value is undefined in C and was capable of corrupting PCM
		 * when compilers optimized this conversion aggressively. */
		raw = unsigned_8bit ? (uint32_t)(p[0] ^ UINT8_C(0x80)) : p[0];
		return raw << 24;
	}
	if (sample_size == 2) {
		raw = bigendian ? ((uint32_t)p[0] << 8) | p[1] :
			(uint32_t)p[0] | ((uint32_t)p[1] << 8);
		return raw << 16;
	}
	if (sample_size == 3) {
		raw = bigendian ? ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2] :
			(uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
		if (raw & UINT32_C(0x00800000)) raw |= UINT32_C(0xff000000);
		return raw << 8;
	}
	if (sample_size != 4) return 0;
	return bigendian ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3] : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
