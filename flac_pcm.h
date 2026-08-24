#ifndef FLAC_PCM_H
#define FLAC_PCM_H

#include <stdint.h>

/* Align signed FLAC PCM to the player's fixed-point sample width. */
static inline int32_t flac_scale_sample(int32_t sample, unsigned source_bits, unsigned target_bits) {
	if (source_bits < target_bits) {
		return (int32_t)((int64_t)sample * ((int64_t)1 << (target_bits - source_bits)));
	}
	if (source_bits > target_bits) {
		return sample >> (source_bits - target_bits);
	}
	return sample;
}

#endif
