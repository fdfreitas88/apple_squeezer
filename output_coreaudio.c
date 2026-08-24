/*
 * Native CoreAudio output for Apple Squeezer.
 * Copyright (c) 2026 Felipe Freitas and contributors.
 * Distributed under the GNU General Public License, version 3 or later.
 */

#include "squeezelite.h"

#if COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <mach/mach_time.h>

static log_level loglevel;
static AudioUnit audio_unit;
static AudioDeviceID audio_device;
static unsigned device_rate;
static u8_t *write_ptr;
static bool ca_exclusive;
static bool ca_bitperfect;
static bool ca_dither;
static bool ca_hogged;
static bool ca_listeners_installed;
static volatile bool ca_device_changed;
static volatile unsigned ca_underruns;
static volatile unsigned ca_overloads;
static volatile bool ca_underrun_armed;
static unsigned ca_reopens;
static u32_t ca_last_underrun_log;
static unsigned ca_pipeline_frames;
static unsigned ca_buffer_frames;
static const char *ca_profile = "balanced";
static const char *ca_mode = "native";
static double ca_headroom_db;
static u32_t ca_headroom_gain = FIXED_ONE;
static u32_t ca_dither_state = 0x41535043U;
static uint64_t ca_processed_frames;
static uint64_t ca_clipped_samples;
static bool ca_physical_verified;
static bool ca_volume_verified;
static bool ca_exclusive_verified;
static u32_t ca_last_telemetry_log;
static thread_type ca_monitor_thread;
static volatile bool ca_monitor_running;

extern struct outputstate output;
extern struct decodestate decode;
extern struct buffer *outputbuf;
extern u8_t *silencebuf;
#if DSD
extern u8_t *silencebuf_dsd;
#endif

#define LOCK mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

/* LMS deliberately empties the output buffer on stop and track replacement.
 * That controlled boundary is silence, not callback starvation. */
void coreaudio_note_output_flush(void) {
	ca_underrun_armed = false;
}

static const char *ca_transport(void) {
#if DSD
	return output.outfmt == DOP ? "DoP" : output.outfmt == PCM ? "PCM" : "DSD";
#else
	return "PCM";
#endif
}

static bool ca_property(AudioObjectID object, AudioObjectPropertySelector selector,
		AudioObjectPropertyScope scope, void *value, UInt32 *size) {
	AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMain };
	return AudioObjectGetPropertyData(object, &address, 0, NULL, size, value) == noErr;
}

static bool ca_set_property(AudioObjectID object, AudioObjectPropertySelector selector,
		AudioObjectPropertyScope scope, const void *value, UInt32 size) {
	AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMain };
	return AudioObjectSetPropertyData(object, &address, 0, NULL, size, value) == noErr;
}

static bool ca_param_enabled(const char *params, const char *wanted) {
	size_t length = strlen(wanted);
	const char *p = params;
	while (p && *p) {
		while (*p == ':' || *p == ',' || isspace((unsigned char)*p)) ++p;
		if (!strncmp(p, wanted, length) &&
			(!p[length] || p[length] == ':' || p[length] == ',' || isspace((unsigned char)p[length]))) return true;
		p = strpbrk(p, ":,");
		if (p) ++p;
	}
	return false;
}

static const char *ca_param_value(const char *params, const char *wanted) {
	static char value[32];
	size_t length = strlen(wanted);
	const char *p = params;
	while (p && *p) {
		while (*p == ':' || *p == ',' || isspace((unsigned char)*p)) ++p;
		if (!strncmp(p, wanted, length) && p[length] == '=') {
			const char *start = p + length + 1;
			size_t n = strcspn(start, ":,");
			if (n >= sizeof(value)) n = sizeof(value) - 1;
			memcpy(value, start, n); value[n] = '\0';
			return value;
		}
		p = strpbrk(p, ":,");
		if (p) ++p;
	}
	return NULL;
}

static u32_t ca_random(void) {
	u32_t x = ca_dither_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	ca_dither_state = x;
	return x;
}

/* Add one triangular 24-bit-output LSB while samples are still in S32 format. */
static void ca_apply_tpdf_24(s32_t *samples, frames_t frames) {
	uint64_t count = (uint64_t)frames * 2;
	for (uint64_t i = 0; i < count; ++i) {
		int64_t noise = (int64_t)(ca_random() & 0xffU) - (int64_t)(ca_random() & 0xffU);
		int64_t value = (int64_t)samples[i] + noise;
		if (value > INT32_MAX) { value = INT32_MAX; ++ca_clipped_samples; }
		else if (value < INT32_MIN) { value = INT32_MIN; ++ca_clipped_samples; }
		samples[i] = (s32_t)value;
	}
}

static OSStatus ca_listener(AudioObjectID object, UInt32 count,
		const AudioObjectPropertyAddress addresses[], void *context) {
	UInt32 i;
	(void)object; (void)context;
	for (i = 0; i < count; ++i) {
		if (addresses[i].mSelector == kAudioDeviceProcessorOverload) {
			++ca_overloads;
		} else {
			ca_device_changed = true;
			output.coreaudio_reopen = true;
			wake_controller();
		}
	}
	return noErr;
}

static void ca_listener_change(AudioObjectID object, AudioObjectPropertySelector selector,
		AudioObjectPropertyScope scope, bool add) {
	AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMain };
	if (add) AudioObjectAddPropertyListener(object, &address, ca_listener, NULL);
	else AudioObjectRemovePropertyListener(object, &address, ca_listener, NULL);
}

static void ca_remove_listeners(void) {
	if (!ca_listeners_installed) return;
	ca_listener_change(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, false);
	ca_listener_change(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, false);
	if (audio_device != kAudioObjectUnknown) {
		ca_listener_change(audio_device, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, false);
		ca_listener_change(audio_device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, false);
		ca_listener_change(audio_device, kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, false);
		ca_listener_change(audio_device, kAudioDeviceProcessorOverload, kAudioDevicePropertyScopeOutput, false);
	}
	ca_listeners_installed = false;
}

static void ca_add_listeners(void) {
	ca_listener_change(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, true);
	ca_listener_change(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, true);
	ca_listener_change(audio_device, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, true);
	ca_listener_change(audio_device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, true);
	ca_listener_change(audio_device, kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, true);
	ca_listener_change(audio_device, kAudioDeviceProcessorOverload, kAudioDevicePropertyScopeOutput, true);
	ca_listeners_installed = true;
}

static bool ca_configure_buffer(AudioDeviceID device) {
	AudioValueRange range;
	UInt32 size = sizeof(range), frames;
	if (!ca_property(device, kAudioDevicePropertyBufferFrameSizeRange,
			kAudioObjectPropertyScopeGlobal, &range, &size)) return true;
	if (!strcmp(ca_profile, "lowlatency")) frames = 64;
	else if (!strcmp(ca_profile, "safe")) frames = 512;
	else frames = 256;
	if (frames < range.mMinimum) frames = (UInt32)range.mMinimum;
	if (frames > range.mMaximum) frames = (UInt32)range.mMaximum;
	if (!ca_set_property(device, kAudioDevicePropertyBufferFrameSize,
			kAudioObjectPropertyScopeGlobal, &frames, sizeof(frames))) {
		LOG_WARN("unable to set CoreAudio buffer profile %s (%u frames)", ca_profile, frames);
	}
	size = sizeof(ca_buffer_frames);
	ca_property(device, kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal, &ca_buffer_frames, &size);
	return true;
}

static bool ca_physical_format(AudioDeviceID device, unsigned requested, bool strict) {
	AudioObjectPropertyAddress address = { kAudioDevicePropertyStreams,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	UInt32 size = 0, i, count;
	AudioStreamID *streams;
	bool valid = false;
	if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != noErr || !size) return false;
	streams = malloc(size);
	if (!streams) return false;
	if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, streams) != noErr) { free(streams); return false; }
	count = size / sizeof(*streams);
	for (i = 0; i < count; ++i) {
		AudioStreamBasicDescription f = {0};
		UInt32 fsize = sizeof(f);
		if (ca_property(streams[i], kAudioStreamPropertyPhysicalFormat,
				kAudioObjectPropertyScopeGlobal, &f, &fsize)) {
			bool pcm = f.mFormatID == kAudioFormatLinearPCM;
			bool rate = f.mSampleRate + 0.5 >= requested && f.mSampleRate - 0.5 <= requested;
			bool stereo = f.mChannelsPerFrame == 2;
			bool integer = !!(f.mFormatFlags & kAudioFormatFlagIsSignedInteger);
			bool noninterleaved = !!(f.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
			bool bytes_valid = f.mBytesPerFrame != 0 && f.mFramesPerPacket != 0;
			LOG_INFO("CoreAudio physical stream %u: %.0f Hz, %u ch, %u-bit, format=%c%c%c%c flags=0x%x",
				(unsigned)streams[i], f.mSampleRate, f.mChannelsPerFrame, f.mBitsPerChannel,
				(char)(f.mFormatID >> 24), (char)(f.mFormatID >> 16), (char)(f.mFormatID >> 8),
				(char)f.mFormatID, (unsigned)f.mFormatFlags);
			LOG_INFO("CoreAudio physical layout: bytes/frame=%u frames/packet=%u bytes/packet=%u interleaved=%s",
				f.mBytesPerFrame, f.mFramesPerPacket, f.mBytesPerPacket, noninterleaved ? "no" : "yes");
			if (pcm && rate && stereo && bytes_valid && (!strict || (integer && f.mBitsPerChannel >= 24))) valid = true;
		}
	}
	free(streams);
	if (!valid) LOG_ERROR("CoreAudio physical stream does not satisfy %u Hz stereo PCM%s",
		requested, strict ? " integer bit-perfect requirements" : "");
	return valid;
}

static bool ca_verify_hardware_controls(AudioDeviceID device, bool enforce_unity) {
	AudioObjectPropertyAddress volume = { kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	AudioObjectPropertyAddress mute = { kAudioDevicePropertyMute,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	UInt32 size; Float32 scalar = 1.0f; UInt32 muted = 0; Boolean settable = false; bool has_volume, has_mute, ok = true;
	has_volume = AudioObjectHasProperty(device, &volume);
	has_mute = AudioObjectHasProperty(device, &mute);
	if (enforce_unity && has_volume && AudioObjectIsPropertySettable(device, &volume, &settable) == noErr && settable)
		(void)AudioObjectSetPropertyData(device, &volume, 0, NULL, sizeof(scalar), &scalar);
	settable = false;
	if (enforce_unity && has_mute && AudioObjectIsPropertySettable(device, &mute, &settable) == noErr && settable)
		(void)AudioObjectSetPropertyData(device, &mute, 0, NULL, sizeof(muted), &muted);
	if (has_volume) { size=sizeof(scalar); ok=AudioObjectGetPropertyData(device,&volume,0,NULL,&size,&scalar)==noErr&&scalar>.9999f; }
	if (has_mute) { size=sizeof(muted); ok=ok&&AudioObjectGetPropertyData(device,&mute,0,NULL,&size,&muted)==noErr&&!muted; }
	LOG_INFO("CoreAudio hardware controls: volume=%s mute=%s unity=%s",
		has_volume?"present":"fixed",has_mute?"present":"absent",ok?"verified":"unverified");
	return ok;
}

static void ca_measure_latency(void) {
	UInt32 device_latency = 0, safety = 0, size = sizeof(UInt32);
	Float64 unit_latency = 0;
	ca_property(audio_device, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput, &device_latency, &size);
	size = sizeof(UInt32);
	ca_property(audio_device, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput, &safety, &size);
	size = sizeof(Float64);
	if (audio_unit) AudioUnitGetProperty(audio_unit, kAudioUnitProperty_Latency,
		kAudioUnitScope_Global, 0, &unit_latency, &size);
	ca_pipeline_frames = device_latency + safety + ca_buffer_frames + (unsigned)(unit_latency * device_rate + 0.5);
	LOG_INFO("CoreAudio latency: device=%u safety=%u buffer=%u unit=%.3fms total=%u frames",
		device_latency, safety, ca_buffer_frames, unit_latency * 1000.0, ca_pipeline_frames);
}

static void *ca_monitor(void *unused) {
	(void)unused;
	while (ca_monitor_running) {
		if (output.error_opening || ca_device_changed) {
			output.coreaudio_reopen = true;
			wake_controller();
		}
		usleep(2000000);
	}
	return NULL;
}

static bool ca_set_nominal_rate(AudioDeviceID device, unsigned requested, unsigned *actual) {
	Float64 rate = requested;
	UInt32 size = sizeof(rate);
	if (!ca_set_property(device, kAudioDevicePropertyNominalSampleRate,
			kAudioObjectPropertyScopeGlobal, &rate, sizeof(rate))) {
		LOG_ERROR("unable to set CoreAudio hardware rate to %u Hz", requested);
		return false;
	}
	/* USB devices normally switch synchronously, but allow the HAL a short bounded settling period. */
	for (unsigned attempt = 0; attempt < 20; ++attempt) {
		size = sizeof(rate);
		if (ca_property(device, kAudioDevicePropertyNominalSampleRate,
				kAudioObjectPropertyScopeGlobal, &rate, &size) && rate + 0.5 >= requested && rate - 0.5 <= requested) {
			*actual = (unsigned)(rate + 0.5);
			return true;
		}
		usleep(10000);
	}
	*actual = (unsigned)(rate + 0.5);
	LOG_ERROR("CoreAudio hardware rate mismatch: requested %u Hz, actual %u Hz", requested, *actual);
	return false;
}

static bool ca_acquire_hog(AudioDeviceID device) {
	pid_t owner = getpid();
	UInt32 size = sizeof(owner);
	if (!ca_set_property(device, kAudioDevicePropertyHogMode,
			kAudioObjectPropertyScopeGlobal, &owner, sizeof(owner))) return false;
	owner = -1;
	if (!ca_property(device, kAudioDevicePropertyHogMode,
			kAudioObjectPropertyScopeGlobal, &owner, &size) || owner != getpid()) return false;
	ca_hogged = true;
	return true;
}

static void ca_release_hog(void) {
	if (ca_hogged && audio_device != kAudioObjectUnknown) {
		pid_t owner = -1;
		ca_set_property(audio_device, kAudioDevicePropertyHogMode,
			kAudioObjectPropertyScopeGlobal, &owner, sizeof(owner));
	}
	ca_hogged = false;
}

static char *ca_device_name(AudioDeviceID device, char *buffer, size_t length) {
	CFStringRef name = NULL;
	UInt32 size = sizeof(name);
	if (!ca_property(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name, &size) || !name) {
		snprintf(buffer, length, "device-%u", (unsigned)device);
		return buffer;
	}
	if (!CFStringGetCString(name, buffer, length, kCFStringEncodingUTF8)) {
		snprintf(buffer, length, "device-%u", (unsigned)device);
	}
	CFRelease(name);
	return buffer;
}

static bool ca_has_output(AudioDeviceID device) {
	AudioObjectPropertyAddress address = {
		kAudioDevicePropertyStreamConfiguration,
		kAudioDevicePropertyScopeOutput,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	AudioBufferList *list;
	UInt32 channels = 0;
	UInt32 i;
	if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != noErr || !size) return false;
	list = malloc(size);
	if (!list) return false;
	if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, list) == noErr) {
		for (i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
	}
	free(list);
	return channels >= 2;
}

static AudioDeviceID ca_find_device(const char *requested) {
	AudioDeviceID device = kAudioObjectUnknown;
	UInt32 size;
	if (!requested || !strncmp(requested, "default", 7)) {
		size = sizeof(device);
		ca_property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
			kAudioObjectPropertyScopeGlobal, &device, &size);
		return device;
	}

	AudioObjectPropertyAddress address = {
		kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
	};
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr) return device;
	AudioDeviceID *devices = malloc(size);
	if (!devices) return device;
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) == noErr) {
		UInt32 count = size / sizeof(*devices), i;
		for (i = 0; i < count; ++i) {
			char name[256];
			ca_device_name(devices[i], name, sizeof(name));
			if (ca_has_output(devices[i]) && (!strcmp(requested, name) || (unsigned)atoi(requested) == devices[i])) {
				device = devices[i];
				break;
			}
		}
	}
	free(devices);
	return device;
}

void list_devices(void) {
	AudioObjectPropertyAddress address = {
		kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr) return;
	AudioDeviceID *devices = malloc(size);
	if (!devices) return;
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) == noErr) {
		UInt32 count = size / sizeof(*devices), i;
		printf("CoreAudio output devices:\n");
		for (i = 0; i < count; ++i) if (ca_has_output(devices[i])) {
			char name[256];
			printf("  %u - %s\n", (unsigned)devices[i], ca_device_name(devices[i], name, sizeof(name)));
		}
		printf("\n");
	}
	free(devices);
}

void set_volume(unsigned left, unsigned right) {
	if (ca_bitperfect) {
		left = right = FIXED_ONE;
	}
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}

bool test_open(const char *device, unsigned rates[], bool userdef_rates) {
	unsigned ref[] TEST_RATES;
	unsigned i, count = 0;
	AudioDeviceID id = ca_find_device(device);
	AudioObjectPropertyAddress address = {
		kAudioDevicePropertyAvailableNominalSampleRates,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (id == kAudioObjectUnknown || !ca_has_output(id)) return false;
	if (userdef_rates) return true;
	if (AudioObjectGetPropertyDataSize(id, &address, 0, NULL, &size) != noErr) return false;
	AudioValueRange *ranges = malloc(size);
	if (!ranges) return false;
	if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, ranges) == noErr) {
		UInt32 range_count = size / sizeof(*ranges), r;
		for (i = 0; ref[i] && count < MAX_SUPPORTED_SAMPLERATES - 1; ++i) {
			for (r = 0; r < range_count; ++r) {
				if (ref[i] >= ranges[r].mMinimum && ref[i] <= ranges[r].mMaximum) {
					rates[count++] = ref[i];
					break;
				}
			}
		}
	}
	free(ranges);
	rates[count] = 0;
	return count != 0;
}

static int ca_write_frames(frames_t frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
		s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr) {
	if (!silence) {
		if (!ca_bitperfect && ca_headroom_gain != FIXED_ONE) {
			gainL = (s32_t)(((int64_t)gainL * ca_headroom_gain) / FIXED_ONE);
			gainR = (s32_t)(((int64_t)gainR * ca_headroom_gain) / FIXED_ONE);
		}
		if (!ca_bitperfect && output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr)
			_apply_cross(outputbuf, frames, cross_gain_in, cross_gain_out, cross_ptr);
		if (!ca_bitperfect && (gainL != FIXED_ONE || gainR != FIXED_ONE)) _apply_gain(outputbuf, frames, gainL, gainR, flags);
		IF_DSD(
			if (output.outfmt == DOP) update_dop((u32_t *)outputbuf->readp, frames, output.invert);
			else if (output.outfmt != PCM && output.invert) dsd_invert((u32_t *)outputbuf->readp, frames);
		)
		IF_DSD(if (output.outfmt == PCM && ca_dither) ca_apply_tpdf_24((s32_t *)outputbuf->readp, frames);)
#if !DSD
		if (ca_dither) ca_apply_tpdf_24((s32_t *)outputbuf->readp, frames);
#endif
		ca_processed_frames += frames;
		memcpy(write_ptr, outputbuf->readp, frames * BYTES_PER_FRAME);
	} else {
		u8_t *buffer = silencebuf;
		IF_DSD(
			if (output.outfmt != PCM) {
				buffer = silencebuf_dsd;
				update_dop((u32_t *)buffer, frames, false);
			}
		)
		memcpy(write_ptr, buffer, frames * BYTES_PER_FRAME);
	}
	write_ptr += frames * BYTES_PER_FRAME;
	return (int)frames;
}

static OSStatus ca_render(void *context, AudioUnitRenderActionFlags *flags,
		const AudioTimeStamp *timestamp, UInt32 bus, UInt32 wanted, AudioBufferList *buffers) {
	frames_t frames;
	UInt32 remaining = wanted;
	uint64_t host_now;
	(void)context; (void)flags; (void)timestamp; (void)bus;
	if (!buffers->mNumberBuffers || !buffers->mBuffers[0].mData) return noErr;
	write_ptr = buffers->mBuffers[0].mData;
	LOCK;
	if (ca_bitperfect) {
		output.fade = FADE_INACTIVE;
		output.fade_mode = FADE_NONE;
		output.current_replay_gain = 0;
		output.gainL = output.gainR = FIXED_ONE;
	}
	output.device_frames = ca_pipeline_frames;
#if DSP
	/* Include FIR and fractional-delay latency in LMS play-point accounting. */
	output.device_frames += dsp_latency_frames();
#endif
#if RESAMPLE
	output.device_frames += resample_latency_frames();
#endif
	host_now = mach_absolute_time();
	if ((timestamp->mFlags & kAudioTimeStampHostTimeValid) && timestamp->mHostTime > host_now) {
		mach_timebase_info_data_t info;
		mach_timebase_info(&info);
		uint64_t ticks = timestamp->mHostTime - host_now;
		uint64_t ns = ticks * info.numer / info.denom;
		output.device_frames += (unsigned)(ns * output.current_sample_rate / 1000000000ULL);
	}
	output.updated = gettime_ms();
	if (output.updated - ca_last_telemetry_log >= 10000) {
		LOG_INFO("CoreAudio telemetry: mode=%s transport=%s rate=%u buffer=%u latency=%u underruns=%u overloads=%u reopens=%u processed=%llu clipped=%llu physical=%s volume=%s exclusive=%s",
			ca_mode, ca_transport(), output.current_sample_rate, ca_buffer_frames, ca_pipeline_frames, ca_underruns,
			ca_overloads, ca_reopens,
			(unsigned long long)ca_processed_frames, (unsigned long long)ca_clipped_samples,
			ca_physical_verified?"verified":"unverified", ca_volume_verified?"verified":"unverified",
			ca_exclusive_verified?"verified":"not-requested");
		ca_last_telemetry_log = output.updated;
	}
	output.frames_played_dmp = output.frames_played;
	/* Count a starvation episode only after this stream has delivered audio.
	 * Explicit flushes and device reopens disarm detection, and a continuous
	 * empty interval counts once rather than once per CoreAudio callback. */
	frames_t buffered_frames = _buf_used(outputbuf) / BYTES_PER_FRAME;
	if (output.state == OUTPUT_RUNNING && decode.state == DECODE_RUNNING &&
			buffered_frames == 0 && ca_underrun_armed) {
		++ca_underruns;
		ca_underrun_armed = false;
		if (output.updated - ca_last_underrun_log >= 5000) {
			LOG_WARN("CoreAudio underrun: total=%u profile=%s buffer=%u frames",
				ca_underruns, ca_profile, ca_buffer_frames);
			ca_last_underrun_log = output.updated;
		}
	}
	do {
		frames = _output_frames(remaining);
		if (frames && buffered_frames && output.state == OUTPUT_RUNNING) ca_underrun_armed = true;
		remaining -= frames;
	} while (remaining && frames);
	if (remaining) memset(write_ptr, 0, remaining * BYTES_PER_FRAME);
	if (output.state == OUTPUT_OFF || device_rate != output.current_sample_rate) {
		output.coreaudio_reopen = true;
		wake_controller();
	}
	UNLOCK;
	return noErr;
}

static void ca_dispose(void) {
	ca_underrun_armed = false;
	ca_remove_listeners();
	if (audio_unit) {
		AudioOutputUnitStop(audio_unit);
		AudioUnitUninitialize(audio_unit);
		AudioComponentInstanceDispose(audio_unit);
		audio_unit = NULL;
	}
	ca_release_hog();
	audio_device = kAudioObjectUnknown;
}

void _coreaudio_open(void) {
	AudioComponentDescription description = {
		kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0
	};
	AudioStreamBasicDescription format = {0};
	AURenderCallbackStruct callback = { ca_render, NULL };
	AudioComponent component;
	OSStatus status;
	ca_dispose();
	ca_physical_verified = ca_volume_verified = ca_exclusive_verified = false;
	if (output.state == OUTPUT_OFF) return;
	++ca_reopens;
	audio_device = ca_find_device(output.device);
	component = AudioComponentFindNext(NULL, &description);
	if (!component || audio_device == kAudioObjectUnknown) goto failed;
	if (ca_exclusive && !ca_acquire_hog(audio_device)) {
		LOG_ERROR("unable to acquire exclusive access to CoreAudio device %u", (unsigned)audio_device);
		goto failed;
	}
	ca_exclusive_verified = ca_exclusive && ca_hogged;
	if (!ca_set_nominal_rate(audio_device, output.current_sample_rate, &device_rate)) goto failed;
	ca_configure_buffer(audio_device);
	if (AudioComponentInstanceNew(component, &audio_unit) != noErr) goto failed;
	UInt32 enable = 1, disable = 0;
	if (AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable, sizeof(enable)) != noErr) goto failed;
	AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable, sizeof(disable));
	if (AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &audio_device, sizeof(audio_device)) != noErr) goto failed;
	format.mSampleRate = output.current_sample_rate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
	format.mBytesPerPacket = BYTES_PER_FRAME;
	format.mFramesPerPacket = 1;
	format.mBytesPerFrame = BYTES_PER_FRAME;
	format.mChannelsPerFrame = 2;
	format.mBitsPerChannel = 32;
	if (AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)) != noErr) goto failed;
	{
		AudioStreamBasicDescription actual = {0};
		UInt32 actual_size = sizeof(actual);
		if (AudioUnitGetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
				&actual, &actual_size) != noErr || actual.mFormatID != kAudioFormatLinearPCM ||
				actual.mSampleRate + 0.5 < format.mSampleRate || actual.mSampleRate - 0.5 > format.mSampleRate ||
				actual.mChannelsPerFrame != 2 || actual.mBitsPerChannel != 32 ||
				actual.mBytesPerFrame != BYTES_PER_FRAME ||
				!(actual.mFormatFlags & kAudioFormatFlagIsSignedInteger)) {
			LOG_ERROR("CoreAudio AUHAL client stream format verification failed");
			goto failed;
		}
	}
	if (AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr) goto failed;
	if ((status = AudioUnitInitialize(audio_unit)) != noErr || (status = AudioOutputUnitStart(audio_unit)) != noErr) goto failed;
	ca_physical_verified = ca_physical_format(audio_device, output.current_sample_rate, ca_bitperfect);
	if (!ca_physical_verified) goto failed;
	ca_volume_verified = ca_verify_hardware_controls(audio_device, ca_bitperfect);
	if (ca_bitperfect && !ca_volume_verified) { LOG_ERROR("bit-perfect mode cannot verify unity hardware volume"); goto failed; }
	ca_measure_latency();
	ca_add_listeners();
	output.error_opening = false;
	ca_device_changed = false;
	LOG_INFO("signal path: mode=%s -> S32 %s%s -> CoreAudio %u Hz -> device %u%s",
		ca_mode, ca_transport(), ca_dither ? " + TPDF24" : "", output.current_sample_rate,
		(unsigned)audio_device, ca_bitperfect ? " [bit-perfect]" : "");
	LOG_INFO("opened CoreAudio device %u: requested=%u Hz hardware=%u Hz profile=%s%s%s",
		(unsigned)audio_device, output.current_sample_rate, device_rate,
		ca_profile, ca_exclusive ? " exclusive" : "", ca_bitperfect ? " bit-perfect" : "");
	return;
failed:
	ca_dispose();
	output.error_opening = true;
	LOG_ERROR("unable to open CoreAudio output");
}

void output_init_coreaudio(log_level level, const char *device, unsigned output_buf_size, char *params,
		unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;
	ca_exclusive = ca_param_enabled(params, "exclusive");
	ca_bitperfect = ca_param_enabled(params, "bitperfect");
	ca_dither = ca_param_enabled(params, "dither");
	const char *mode = ca_param_value(params, "mode");
	if (mode && (!strcmp(mode, "native") || !strcmp(mode, "pcm-studio"))) {
		ca_mode = !strcmp(mode, "pcm-studio") ? "pcm-studio" : "native";
	} else if (mode) {
		LOG_WARN("unknown CoreAudio mode '%s'; using native", mode);
		ca_mode = "native";
	}
	const char *headroom = ca_param_value(params, "headroom");
	ca_headroom_db = headroom ? strtod(headroom, NULL) : 0.0;
	if (ca_headroom_db < 0.0 || ca_headroom_db > 12.0) {
		LOG_WARN("invalid headroom %.2f dB; using 0 dB", ca_headroom_db);
		ca_headroom_db = 0.0;
	}
	ca_headroom_gain = to_gain((float)pow(10.0, -ca_headroom_db / 20.0));
	if (ca_bitperfect) {
		ca_dither = false;
		ca_headroom_db = 0.0;
		ca_headroom_gain = FIXED_ONE;
	}
	const char *profile = ca_param_value(params, "profile");
	if (profile && (!strcmp(profile, "safe") || !strcmp(profile, "balanced") || !strcmp(profile, "lowlatency"))) {
		ca_profile = !strcmp(profile, "safe") ? "safe" : !strcmp(profile, "lowlatency") ? "lowlatency" : "balanced";
	} else if (profile) {
		LOG_WARN("unknown CoreAudio buffer profile '%s'; using balanced", profile);
		ca_profile = "balanced";
	}
	LOG_INFO("CoreAudio mode: %s, profile=%s%s%s%s, headroom=%.2f dB", ca_mode, ca_profile,
		ca_exclusive ? ", exclusive" : "", ca_bitperfect ? ", bit-perfect fixed-volume" : "",
		ca_dither ? ", TPDF24" : "", ca_headroom_db);
	memset(&output, 0, sizeof(output));
	output.format = S32_LE;
	output.write_cb = ca_write_frames;
	output.rate_delay = rate_delay;
	output_init_common(level, device, output_buf_size, rates, idle);
	ca_monitor_running = true;
	pthread_create(&ca_monitor_thread, NULL, ca_monitor, NULL);
	_coreaudio_open();
}

void output_close_coreaudio(void) {
	ca_monitor_running = false;
	pthread_join(ca_monitor_thread, NULL);
	ca_dispose();
	output_close_common();
}

#endif
