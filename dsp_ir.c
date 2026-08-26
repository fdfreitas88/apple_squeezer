/* Offline WAV/AIFF impulse-response loader for native DSP configuration. */
#include "squeezelite.h"
#if DSP
#include <errno.h>
#include <math.h>
#include <stdio.h>

#define IR_FILE_LIMIT (64U * 1024U * 1024U)
#define IR_TAP_LIMIT 262144U

static unsigned _le16(const u8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned _le32(const u8_t *p) { return _le16(p) | (_le16(p + 2) << 16); }
static unsigned _be16(const u8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned _be32(const u8_t *p) { return (_be16(p) << 16) | _be16(p + 2); }

static void _error(char *out, size_t cap, const char *message) {
	if (out && cap) snprintf(out, cap, "%s", message);
}

static double _extended80(const u8_t *p) {
	unsigned exponent = ((unsigned)(p[0] & 0x7f) << 8) | p[1];
	u64_t mantissa = ((u64_t)_be32(p + 2) << 32) | _be32(p + 6);
	if (!exponent || !mantissa) return 0;
	return ldexp((double)mantissa, (int)exponent - 16383 - 63) * (p[0] & 0x80 ? -1 : 1);
}

static double _pcm(const u8_t *p, unsigned bits, bool little, bool floating) {
	if (floating && bits == 32) {
		u32_t raw = little ? _le32(p) : _be32(p); float value; memcpy(&value, &raw, 4); return value;
	}
	if (floating && bits == 64) {
		u64_t raw = little ? ((u64_t)_le32(p + 4) << 32) | _le32(p) : ((u64_t)_be32(p) << 32) | _be32(p + 4);
		double value; memcpy(&value, &raw, 8); return value;
	}
	if (bits == 16) { int value = little ? (short)_le16(p) : (short)_be16(p); return value / 32768.0; }
	if (bits == 24) {
		int value = little ? (int)(p[0] | (p[1] << 8) | (p[2] << 16)) : (int)((p[0] << 16) | (p[1] << 8) | p[2]);
		if (value & 0x800000) value |= ~0xffffff; return value / 8388608.0;
	}
	if (bits == 32) { int32_t value = (int32_t)(little ? _le32(p) : _be32(p)); return value / 2147483648.0; }
	return 0;
}

bool dsp_ir_load(const char *path, double **left, double **right, unsigned *taps,
	unsigned *sample_rate, char *error, size_t error_capacity) {
	FILE *file; u8_t *data = NULL; long length; size_t offset, data_offset = 0, data_size = 0;
	unsigned channels = 0, rate = 0, bits = 0, format = 0, frames, i; bool little, floating = false;
	double *l = NULL, *r = NULL, peak = 0;
	if (!path || !left || !right || !taps || !sample_rate) return false;
	*left = *right = NULL; *taps = *sample_rate = 0;
	file = fopen(path, "rb");
	if (!file) { _error(error, error_capacity, strerror(errno)); return false; }
	if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 12 || length > IR_FILE_LIMIT || fseek(file, 0, SEEK_SET)) {
		fclose(file); _error(error, error_capacity, "invalid or oversized impulse file"); return false;
	}
	data = malloc((size_t)length); if (!data) { fclose(file); _error(error, error_capacity, "out of memory"); return false; }
	if (fread(data, 1, (size_t)length, file) != (size_t)length) { free(data); fclose(file); _error(error, error_capacity, "unable to read impulse file"); return false; }
	fclose(file);
	little = !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4);
	if (little) {
		for (offset = 12; offset + 8 <= (size_t)length;) {
			size_t size = _le32(data + offset + 4), body = offset + 8; if (size > (size_t)length - body) break;
			if (!memcmp(data + offset, "fmt ", 4) && size >= 16) { format = _le16(data + body); channels = _le16(data + body + 2); rate = _le32(data + body + 4); bits = _le16(data + body + 14); }
			if (!memcmp(data + offset, "data", 4)) { data_offset = body; data_size = size; }
			if (size + (size & 1) > (size_t)length - body) break;
			offset = body + size + (size & 1);
		}
		floating = format == 3;
		if (format != 1 && format != 3) { _error(error, error_capacity, "WAV impulse must be PCM or IEEE float"); goto fail; }
	} else if (!memcmp(data, "FORM", 4) && (!memcmp(data + 8, "AIFF", 4) || !memcmp(data + 8, "AIFC", 4))) {
		for (offset = 12; offset + 8 <= (size_t)length;) {
			size_t size = _be32(data + offset + 4), body = offset + 8; if (size > (size_t)length - body) break;
			if (!memcmp(data + offset, "COMM", 4) && size >= 18) { channels = _be16(data + body); bits = _be16(data + body + 6); rate = (unsigned)llround(_extended80(data + body + 8)); }
			if (!memcmp(data + offset, "SSND", 4) && size >= 8) {
				unsigned skip = _be32(data + body);
				if ((size_t)skip <= size - 8) { data_offset = body + 8 + skip; data_size = size - 8 - skip; }
			}
			if (size + (size & 1) > (size_t)length - body) break;
			offset = body + size + (size & 1);
		}
		little = false;
	} else { _error(error, error_capacity, "impulse must be WAV, AIFF, or AIFC"); goto fail; }
	if ((channels != 1 && channels != 2) || !rate || !data_offset || !data_size || !bits || bits % 8 || (floating ? bits != 32 && bits != 64 : bits != 16 && bits != 24 && bits != 32)) {
		_error(error, error_capacity, "unsupported impulse format; use mono/stereo 16/24/32-bit PCM or 32/64-bit float"); goto fail;
	}
	frames = (unsigned)(data_size / (channels * (bits / 8)));
	if (!frames || frames > IR_TAP_LIMIT || data_offset + (size_t)frames * channels * (bits / 8) > (size_t)length) { _error(error, error_capacity, "invalid impulse length"); goto fail; }
	l = calloc(frames, sizeof(*l)); r = calloc(frames, sizeof(*r)); if (!l || !r) { _error(error, error_capacity, "out of memory"); goto fail; }
	for (i = 0; i < frames; ++i) {
		const u8_t *p = data + data_offset + (size_t)i * channels * (bits / 8);
		l[i] = _pcm(p, bits, little, floating); r[i] = channels == 2 ? _pcm(p + bits / 8, bits, little, floating) : l[i];
		if (!isfinite(l[i]) || !isfinite(r[i])) { _error(error, error_capacity, "impulse contains non-finite samples"); goto fail; }
		if (fabs(l[i]) > peak) peak = fabs(l[i]); if (fabs(r[i]) > peak) peak = fabs(r[i]);
	}
	if (peak <= 0) { _error(error, error_capacity, "impulse is silent"); goto fail; }
	if (peak > 16.0) { _error(error, error_capacity, "impulse gain is outside the safe processing range"); goto fail; }
	free(data); *left = l; *right = r; *taps = frames; *sample_rate = rate; return true;
fail:
	free(data); free(l); free(r); return false;
}
#endif
