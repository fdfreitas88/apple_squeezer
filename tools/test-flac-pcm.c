#include <stdint.h>
#include <stdio.h>

#include "../flac_pcm.h"

static int check_width(unsigned source_bits, unsigned target_bits) {
	int32_t minimum = source_bits == 32 ? INT32_MIN : -(int32_t)((uint32_t)1 << (source_bits - 1));
	int32_t maximum = source_bits == 32 ? INT32_MAX : (int32_t)(((uint32_t)1 << (source_bits - 1)) - 1);
	int32_t scaled_minimum = flac_scale_sample(minimum, source_bits, target_bits);
	int32_t scaled_maximum = flac_scale_sample(maximum, source_bits, target_bits);
	int32_t expected_minimum = target_bits == 32 ? INT32_MIN : -(int32_t)((uint32_t)1 << (target_bits - 1));
	int32_t expected_maximum;

	if (source_bits <= target_bits) {
		expected_maximum = (int32_t)((int64_t)maximum << (target_bits - source_bits));
	} else {
		expected_maximum = target_bits == 32 ? INT32_MAX : (int32_t)(((uint32_t)1 << (target_bits - 1)) - 1);
	}

	if (scaled_minimum != expected_minimum || scaled_maximum != expected_maximum ||
		flac_scale_sample(0, source_bits, target_bits) != 0) {
		fprintf(stderr, "failed %u-bit to %u-bit: min=%d max=%d\n",
				source_bits, target_bits, scaled_minimum, scaled_maximum);
		return 1;
	}
	return 0;
}

int main(void) {
	for (unsigned bits = 4; bits <= 32; ++bits) {
		if (check_width(bits, 16) || check_width(bits, 32)) return 1;
	}
	puts("FLAC PCM conversion passed for every bit depth from 4 through 32");
	return 0;
}
