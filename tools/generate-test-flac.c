#include <FLAC/stream_encoder.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_FRAMES 4096

int main(int argc, char **argv) {
	FLAC__StreamEncoder *encoder;
	FLAC__int32 *samples;
	unsigned rate, seconds;
	uint64_t total, frame = 0;
	int dop = 0;
	int ok = 0;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s OUTPUT.flac RATE SECONDS [dop]\n", argv[0]);
		return 2;
	}
	rate = (unsigned)strtoul(argv[2], NULL, 10);
	seconds = (unsigned)strtoul(argv[3], NULL, 10);
	if (argc == 5) {
		if (strcmp(argv[4], "dop") != 0) return 2;
		dop = 1;
	}
	if (rate < 8000 || rate > 768000 || seconds == 0 || seconds > 86400) return 2;
	if (dop && rate != 176400) return 2;
	total = (uint64_t)rate * seconds;
	encoder = FLAC__stream_encoder_new();
	samples = malloc(BLOCK_FRAMES * 2 * sizeof(*samples));
	if (!encoder || !samples) goto done;
	if (!FLAC__stream_encoder_set_channels(encoder, 2) ||
		!FLAC__stream_encoder_set_bits_per_sample(encoder, 24) ||
		!FLAC__stream_encoder_set_sample_rate(encoder, rate) ||
		!FLAC__stream_encoder_set_compression_level(encoder, 5) ||
		!FLAC__stream_encoder_set_total_samples_estimate(encoder, total) ||
		FLAC__stream_encoder_init_file(encoder, argv[1], NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) goto done;
	while (frame < total) {
		unsigned count = (unsigned)((total - frame) > BLOCK_FRAMES ? BLOCK_FRAMES : (total - frame));
		unsigned i;
		for (i = 0; i < count; ++i) {
			FLAC__int32 value;
			if (dop) {
				unsigned marker = ((frame + i) & 1) ? 0xFA : 0x05;
				unsigned packed = (marker << 16) | 0x6969;
				value = (packed & 0x800000) ? (FLAC__int32)(packed - 0x1000000) : (FLAC__int32)packed;
			} else {
				double phase = 2.0 * M_PI * 997.0 * (double)(frame + i) / rate;
				value = (FLAC__int32)(sin(phase) * 1048575.0);
			}
			samples[i * 2] = value;
			samples[i * 2 + 1] = value;
		}
		if (!FLAC__stream_encoder_process_interleaved(encoder, samples, count)) goto done;
		frame += count;
	}
	ok = FLAC__stream_encoder_finish(encoder) ? 1 : 0;
done:
	if (encoder) FLAC__stream_encoder_delete(encoder);
	free(samples);
	if (!ok) fprintf(stderr, "unable to generate FLAC fixture %s\n", argc > 1 ? argv[1] : "");
	return ok ? 0 : 1;
}
