/*
 * Native CoreAudio output for Apple Squeezer.
 * Copyright (c) 2026 Felipe Freitas and contributors.
 * Distributed under the GNU General Public License, version 3 or later.
 */

#include "squeezelite.h"
#include "pcm_convert.h"

#if COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <mach/mach_time.h>

static log_level loglevel;
static AudioUnit audio_unit;
static AudioDeviceID audio_device;
static unsigned device_rate;
static u8_t *write_ptr;
static bool ca_exclusive;
static bool ca_float_client;
static bool ca_bitperfect;
/* the modes requested on the command line; ca_exclusive/ca_float_client/ca_bitperfect are the
 * ones in effect for the current open and may have been relaxed by the shared fallback */
static bool ca_req_exclusive;
static bool ca_req_float_client;
static bool ca_req_bitperfect;
static bool ca_allow_fallback = true;
static bool ca_fallback_active;
/* consecutive failed opens and when the last one failed: the monitor backs off instead of
 * retrying every 200 ms (28k "unable to open" lines in two hours on 2026-09-01) */
static unsigned ca_open_failures;
static u32_t ca_open_fail_ms;
static bool ca_dither;
static bool ca_hogged;
static bool ca_listeners_installed;
static bool ca_device_changed;
static unsigned ca_underruns;
static unsigned ca_overloads;
static bool ca_underrun_armed;
static unsigned ca_reopens;
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
static bool ca_monitor_running;
static bool ca_monitor_started;
static bool ca_in_render;
static bool ca_wake_deferred;
static mach_timebase_info_data_t ca_timebase;

static struct {
	AudioDeviceID device;
	Float64 rate;
	UInt32 buffer_frames;
	Float32 volume;
	UInt32 mute;
	bool have_rate, have_buffer, have_volume, have_mute;
	bool changed_rate, changed_buffer, changed_volume, changed_mute;
} ca_saved;

static void ca_release_hog(void);

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
	__atomic_store_n(&ca_underrun_armed, false, __ATOMIC_RELEASE);
}

bool coreaudio_rendering(void) {
	return __atomic_load_n(&ca_in_render, __ATOMIC_ACQUIRE);
}

void coreaudio_defer_wake(void) {
	__atomic_store_n(&ca_wake_deferred, true, __ATOMIC_RELEASE);
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

static void ca_snapshot_device_state(AudioDeviceID device) {
	AudioObjectPropertyAddress volume = { kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	AudioObjectPropertyAddress mute = { kAudioDevicePropertyMute,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	UInt32 size;
	memset(&ca_saved, 0, sizeof(ca_saved));
	ca_saved.device = device;
	size = sizeof(ca_saved.rate);
	ca_saved.have_rate = ca_property(device, kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal, &ca_saved.rate, &size);
	size = sizeof(ca_saved.buffer_frames);
	ca_saved.have_buffer = ca_property(device, kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal, &ca_saved.buffer_frames, &size);
	size = sizeof(ca_saved.volume);
	ca_saved.have_volume = AudioObjectHasProperty(device, &volume) &&
		AudioObjectGetPropertyData(device, &volume, 0, NULL, &size, &ca_saved.volume) == noErr;
	size = sizeof(ca_saved.mute);
	ca_saved.have_mute = AudioObjectHasProperty(device, &mute) &&
		AudioObjectGetPropertyData(device, &mute, 0, NULL, &size, &ca_saved.mute) == noErr;
}

static void ca_restore_device_state(void) {
	AudioObjectPropertyAddress volume = { kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	AudioObjectPropertyAddress mute = { kAudioDevicePropertyMute,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	Float64 rate;
	Float32 scalar;
	UInt32 value, size;
	if (!ca_saved.device || ca_saved.device != audio_device) return;
	if (ca_saved.changed_volume) {
		size = sizeof(scalar);
		if (AudioObjectGetPropertyData(audio_device, &volume, 0, NULL, &size, &scalar) == noErr && scalar > .9999f)
			(void)AudioObjectSetPropertyData(audio_device, &volume, 0, NULL, sizeof(ca_saved.volume), &ca_saved.volume);
	}
	if (ca_saved.changed_mute) {
		size = sizeof(value);
		if (AudioObjectGetPropertyData(audio_device, &mute, 0, NULL, &size, &value) == noErr && !value)
			(void)AudioObjectSetPropertyData(audio_device, &mute, 0, NULL, sizeof(ca_saved.mute), &ca_saved.mute);
	}
	if (ca_saved.changed_buffer) {
		size = sizeof(value);
		if (ca_property(audio_device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &value, &size) && value == __atomic_load_n(&ca_buffer_frames, __ATOMIC_ACQUIRE))
			(void)ca_set_property(audio_device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
				&ca_saved.buffer_frames, sizeof(ca_saved.buffer_frames));
	}
	if (ca_saved.changed_rate) {
		size = sizeof(rate);
		if (ca_property(audio_device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate, &size) && fabs(rate - __atomic_load_n(&device_rate, __ATOMIC_ACQUIRE)) < .5)
			(void)ca_set_property(audio_device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
				&ca_saved.rate, sizeof(ca_saved.rate));
	}
	memset(&ca_saved, 0, sizeof(ca_saved));
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
		if (value > INT32_MAX) { value = INT32_MAX; __atomic_add_fetch(&ca_clipped_samples, 1ULL, __ATOMIC_RELAXED); }
		else if (value < INT32_MIN) { value = INT32_MIN; __atomic_add_fetch(&ca_clipped_samples, 1ULL, __ATOMIC_RELAXED); }
		samples[i] = (s32_t)value;
	}
}

static OSStatus ca_listener(AudioObjectID object, UInt32 count,
		const AudioObjectPropertyAddress addresses[], void *context) {
	UInt32 i;
	(void)object; (void)context;
	for (i = 0; i < count; ++i) {
		if (addresses[i].mSelector == kAudioDeviceProcessorOverload) {
				__atomic_add_fetch(&ca_overloads, 1U, __ATOMIC_RELAXED);
			} else {
				__atomic_store_n(&ca_device_changed, true, __ATOMIC_RELEASE);
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
		ca_listener_change(audio_device, kAudioHardwareServiceDeviceProperty_VirtualMainVolume, kAudioDevicePropertyScopeOutput, false);
		ca_listener_change(audio_device, kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput, false);
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
	ca_listener_change(audio_device, kAudioHardwareServiceDeviceProperty_VirtualMainVolume, kAudioDevicePropertyScopeOutput, true);
	ca_listener_change(audio_device, kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput, true);
	ca_listeners_installed = true;
}

static bool ca_configure_buffer(AudioDeviceID device) {
	AudioValueRange range;
	UInt32 size = sizeof(range), frames, actual = 0;
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
	size = sizeof(actual);
	ca_property(device, kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal, &actual, &size);
	__atomic_store_n(&ca_buffer_frames, actual, __ATOMIC_RELEASE);
	ca_saved.changed_buffer = ca_saved.have_buffer && actual != ca_saved.buffer_frames;
	return true;
}

static bool ca_physical_format(AudioDeviceID device, unsigned requested, bool strict) {
	AudioObjectPropertyAddress address = { kAudioDevicePropertyStreams,
		kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
	UInt32 size = 0, i, count;
	AudioStreamID *streams;
	bool valid = false;
	unsigned valid_count = 0;
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
			bool packed_layout = !noninterleaved && f.mFramesPerPacket == 1 &&
				f.mBytesPerPacket == f.mBytesPerFrame &&
				(((f.mFormatFlags & kAudioFormatFlagIsPacked) &&
				  f.mBytesPerFrame == f.mChannelsPerFrame * ((f.mBitsPerChannel + 7U) / 8U)) ||
				 ((f.mFormatFlags & kAudioFormatFlagIsAlignedHigh) && f.mChannelsPerFrame &&
				  !(f.mBytesPerFrame % f.mChannelsPerFrame) &&
				  (f.mBytesPerFrame / f.mChannelsPerFrame) * 8U >= f.mBitsPerChannel));
			LOG_INFO("CoreAudio physical stream %u: %.0f Hz, %u ch, %u-bit, format=%c%c%c%c flags=0x%x",
				(unsigned)streams[i], f.mSampleRate, f.mChannelsPerFrame, f.mBitsPerChannel,
				(char)(f.mFormatID >> 24), (char)(f.mFormatID >> 16), (char)(f.mFormatID >> 8),
				(char)f.mFormatID, (unsigned)f.mFormatFlags);
			LOG_INFO("CoreAudio physical layout: bytes/frame=%u frames/packet=%u bytes/packet=%u interleaved=%s",
				f.mBytesPerFrame, f.mFramesPerPacket, f.mBytesPerPacket, noninterleaved ? "no" : "yes");
			if (pcm && rate && stereo && bytes_valid && (!strict || (integer && packed_layout && f.mBitsPerChannel >= 24))) {
				valid = true;
				valid_count++;
			}
		}
	}
	free(streams);
	if (strict && (count != 1 || valid_count != 1)) valid = false;
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
	if (enforce_unity && has_volume && AudioObjectIsPropertySettable(device, &volume, &settable) == noErr && settable) {
		ca_saved.changed_volume = AudioObjectSetPropertyData(device, &volume, 0, NULL, sizeof(scalar), &scalar) == noErr &&
			ca_saved.have_volume && fabsf(ca_saved.volume - scalar) > .0001f;
	}
	settable = false;
	if (enforce_unity && has_mute && AudioObjectIsPropertySettable(device, &mute, &settable) == noErr && settable)
		ca_saved.changed_mute = AudioObjectSetPropertyData(device, &mute, 0, NULL, sizeof(muted), &muted) == noErr &&
			ca_saved.have_mute && ca_saved.mute != muted;
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
	unsigned buffer_frames = __atomic_load_n(&ca_buffer_frames, __ATOMIC_ACQUIRE);
	unsigned rate = __atomic_load_n(&device_rate, __ATOMIC_ACQUIRE);
	unsigned pipeline = device_latency + safety + buffer_frames + (unsigned)(unit_latency * rate + 0.5);
	__atomic_store_n(&ca_pipeline_frames, pipeline, __ATOMIC_RELEASE);
	LOG_INFO("CoreAudio latency: device=%u safety=%u buffer=%u unit=%.3fms total=%u frames",
		device_latency, safety, buffer_frames, unit_latency * 1000.0, pipeline);
}

/* 1 s after the first failed open, doubling up to a 30 s ceiling */
static u32_t ca_retry_delay_ms(unsigned failures) {
	unsigned shift = failures ? failures - 1 : 0;
	u32_t delay;
	if (shift > 5) shift = 5;
	delay = 1000U << shift;
	return delay > 30000U ? 30000U : delay;
}

static void *ca_monitor(void *unused) {
	(void)unused;
	while (__atomic_load_n(&ca_monitor_running, __ATOMIC_ACQUIRE)) {
		if (__atomic_exchange_n(&ca_wake_deferred, false, __ATOMIC_ACQ_REL)) wake_controller();
		{
			bool changed = __atomic_exchange_n(&ca_device_changed, false, __ATOMIC_ACQ_REL);
			bool failed = __atomic_load_n(&output.error_opening, __ATOMIC_ACQUIRE);
			/* a device change is new information: retry at once and restart the backoff */
			if (changed) __atomic_store_n(&ca_open_failures, 0U, __ATOMIC_RELEASE);
			if (changed || (failed && gettime_ms() - __atomic_load_n(&ca_open_fail_ms, __ATOMIC_ACQUIRE) >=
					ca_retry_delay_ms(__atomic_load_n(&ca_open_failures, __ATOMIC_ACQUIRE)))) {
				__atomic_store_n(&output.coreaudio_reopen, true, __ATOMIC_RELEASE);
				wake_controller();
			}
		}
		{
			u32_t now = gettime_ms();
			if (now - ca_last_telemetry_log >= 10000) {
				LOG_INFO("CoreAudio telemetry: mode=%s rate=%u buffer=%u latency=%u underruns=%u overloads=%u reopens=%u processed=%llu clipped=%llu physical=%s volume=%s exclusive=%s fallback=%s",
					ca_mode, __atomic_load_n(&device_rate, __ATOMIC_ACQUIRE), __atomic_load_n(&ca_buffer_frames, __ATOMIC_ACQUIRE), __atomic_load_n(&ca_pipeline_frames, __ATOMIC_ACQUIRE),
					__atomic_load_n(&ca_underruns, __ATOMIC_RELAXED), __atomic_load_n(&ca_overloads, __ATOMIC_RELAXED), __atomic_load_n(&ca_reopens, __ATOMIC_RELAXED),
					(unsigned long long)__atomic_load_n(&ca_processed_frames, __ATOMIC_RELAXED),
					(unsigned long long)__atomic_load_n(&ca_clipped_samples, __ATOMIC_RELAXED),
					__atomic_load_n(&ca_physical_verified,__ATOMIC_ACQUIRE)?"verified":"unverified", __atomic_load_n(&ca_volume_verified,__ATOMIC_ACQUIRE)?"verified":"unverified",
					__atomic_load_n(&ca_exclusive_verified,__ATOMIC_ACQUIRE)?"verified":"not-requested",
					__atomic_load_n(&ca_fallback_active,__ATOMIC_ACQUIRE)?"shared":"none");
				ca_last_telemetry_log = now;
			}
		}
		usleep(200000);
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
	ca_saved.changed_rate = ca_saved.have_rate && fabs(ca_saved.rate - rate) > .5;
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
	ca_hogged = true;
	owner = -1;
	if (!ca_property(device, kAudioDevicePropertyHogMode,
			kAudioObjectPropertyScopeGlobal, &owner, &size) || owner != getpid()) {
		ca_release_hog();
		return false;
	}
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

static bool ca_device_uid(AudioDeviceID device, char *buffer, size_t length) {
	CFStringRef uid = NULL;
	UInt32 size = sizeof(uid);
	if (!buffer || !length || !ca_property(device, kAudioDevicePropertyDeviceUID,
			kAudioObjectPropertyScopeGlobal, &uid, &size) || !uid) return false;
	bool ok = CFStringGetCString(uid, buffer, length, kCFStringEncodingUTF8);
	CFRelease(uid);
	return ok && buffer[0];
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
	if (!requested || !strcmp(requested, "default")) {
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
			char name[256], uid[256] = "";
			ca_device_name(devices[i], name, sizeof(name));
			ca_device_uid(devices[i], uid, sizeof(uid));
			if (ca_has_output(devices[i]) && (!strcmp(requested, name) || !strcmp(requested, uid) ||
				(strspn(requested, "0123456789") == strlen(requested) && (unsigned)strtoul(requested, NULL, 10) == devices[i]))) {
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
			char name[256], uid[256];
			ca_device_name(devices[i], name, sizeof(name));
			if (ca_device_uid(devices[i], uid, sizeof(uid))) printf("  %s - %s\n", uid, name);
			else printf("  %u - %s\n", (unsigned)devices[i], name);
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
		__atomic_add_fetch(&ca_processed_frames, frames, __ATOMIC_RELAXED);
		if (ca_float_client) {
			const u32_t *source = (const u32_t *)outputbuf->readp;
			float *destination = (float *)write_ptr;
			uint64_t samples = (uint64_t)frames * 2U;
			while (samples--) *destination++ = pcm_s32_to_float(*source++);
		} else {
			memcpy(write_ptr, outputbuf->readp, frames * BYTES_PER_FRAME);
		}
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
	/* _output_frames mutates shared ring-buffer and playback state, so the
	 * callback must serialize with the decoder.  A trylock here converted
	 * ordinary, short producer-side critical sections into audible zero-filled
	 * callbacks (roughly two per second during sustained FLAC playback). */
	pthread_mutex_lock(&outputbuf->mutex);
	__atomic_store_n(&ca_in_render, true, __ATOMIC_RELEASE);
	if (ca_bitperfect) {
		output.fade = FADE_INACTIVE;
		output.fade_mode = FADE_NONE;
		output.current_replay_gain = 0;
		output.gainL = output.gainR = FIXED_ONE;
	}
	output.device_frames = __atomic_load_n(&ca_pipeline_frames, __ATOMIC_ACQUIRE);
#if DSP
	/* Include FIR and fractional-delay latency in LMS play-point accounting. */
	output.device_frames += dsp_latency_frames();
#endif
#if RESAMPLE
	output.device_frames += resample_latency_frames();
#endif
	host_now = mach_absolute_time();
	if (timestamp && (timestamp->mFlags & kAudioTimeStampHostTimeValid) && timestamp->mHostTime > host_now && ca_timebase.denom) {
		uint64_t ticks = timestamp->mHostTime - host_now;
		long double ns = (long double)ticks * ca_timebase.numer / ca_timebase.denom;
		output.device_frames += (unsigned)(ns * output.current_sample_rate / 1000000000.0L);
	}
	output.updated = gettime_ms();
	output.frames_played_dmp = output.frames_played;
	/* Count a starvation episode only after this stream has delivered audio.
	 * Explicit flushes and device reopens disarm detection, and a continuous
	 * empty interval counts once rather than once per CoreAudio callback. */
	frames_t buffered_frames = _buf_used(outputbuf) / BYTES_PER_FRAME;
	if (output.state == OUTPUT_RUNNING && decode.state == DECODE_RUNNING &&
			buffered_frames == 0 && __atomic_load_n(&ca_underrun_armed, __ATOMIC_ACQUIRE)) {
		__atomic_add_fetch(&ca_underruns, 1U, __ATOMIC_RELAXED);
		__atomic_store_n(&ca_underrun_armed, false, __ATOMIC_RELEASE);
	}
	do {
		frames = _output_frames(remaining);
		if (frames && buffered_frames && output.state == OUTPUT_RUNNING) __atomic_store_n(&ca_underrun_armed, true, __ATOMIC_RELEASE);
		remaining -= frames;
	} while (remaining && frames);
	if (remaining) memset(write_ptr, 0, remaining * BYTES_PER_FRAME);
	if (output.state == OUTPUT_OFF || __atomic_load_n(&device_rate, __ATOMIC_ACQUIRE) != output.current_sample_rate) {
		__atomic_store_n(&output.coreaudio_reopen, true, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&ca_in_render, false, __ATOMIC_RELEASE);
	UNLOCK;
	return noErr;
}

static void ca_dispose(void) {
	__atomic_store_n(&ca_underrun_armed, false, __ATOMIC_RELEASE);
	ca_remove_listeners();
	if (audio_unit) {
		AudioOutputUnitStop(audio_unit);
		AudioUnitUninitialize(audio_unit);
		AudioComponentInstanceDispose(audio_unit);
		audio_unit = NULL;
	}
	ca_restore_device_state();
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
	bool found = false;
	ca_dispose();
	__atomic_store_n(&ca_physical_verified, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ca_volume_verified, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ca_exclusive_verified, false, __ATOMIC_RELEASE);
	/* every open starts from the requested mode; a fallback applies to this open only */
	ca_exclusive = ca_req_exclusive;
	ca_float_client = ca_req_float_client;
	ca_bitperfect = ca_req_bitperfect;
	__atomic_store_n(&ca_fallback_active, false, __ATOMIC_RELEASE);
	if (output.state == OUTPUT_OFF) return;
	__atomic_add_fetch(&ca_reopens, 1U, __ATOMIC_RELAXED);
retry:
	audio_device = ca_find_device(output.device);
	component = AudioComponentFindNext(NULL, &description);
	if (!component || audio_device == kAudioObjectUnknown) goto failed;
	found = true;
	ca_snapshot_device_state(audio_device);
	if (ca_exclusive && !ca_acquire_hog(audio_device)) {
		LOG_ERROR("unable to acquire exclusive access to CoreAudio device %u", (unsigned)audio_device);
		goto failed;
	}
	__atomic_store_n(&ca_exclusive_verified, ca_exclusive && ca_hogged, __ATOMIC_RELEASE);
	{
		unsigned actual_rate;
		if (!ca_set_nominal_rate(audio_device, output.current_sample_rate, &actual_rate)) goto failed;
		__atomic_store_n(&device_rate, actual_rate, __ATOMIC_RELEASE);
	}
	ca_configure_buffer(audio_device);
	if (AudioComponentInstanceNew(component, &audio_unit) != noErr) goto failed;
	UInt32 enable = 1, disable = 0;
	if (AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable, sizeof(enable)) != noErr) goto failed;
	AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable, sizeof(disable));
	if (AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &audio_device, sizeof(audio_device)) != noErr) goto failed;
	format.mSampleRate = output.current_sample_rate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = (ca_float_client ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
		kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
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
				(ca_float_client ? !(actual.mFormatFlags & kAudioFormatFlagIsFloat) :
					!(actual.mFormatFlags & kAudioFormatFlagIsSignedInteger)) ||
				!(actual.mFormatFlags & kAudioFormatFlagIsPacked) ||
				(actual.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
			LOG_ERROR("CoreAudio AUHAL client stream format verification failed");
			goto failed;
		}
	}
	if (AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr) goto failed;
	status = AudioUnitInitialize(audio_unit);
	if (status != noErr) goto failed;
	status = AudioOutputUnitStart(audio_unit);
	if (status != noErr) goto failed;
	__atomic_store_n(&ca_physical_verified, ca_physical_format(audio_device, output.current_sample_rate, ca_bitperfect), __ATOMIC_RELEASE);
	if (!__atomic_load_n(&ca_physical_verified, __ATOMIC_ACQUIRE)) goto failed;
	__atomic_store_n(&ca_volume_verified, ca_verify_hardware_controls(audio_device, ca_bitperfect), __ATOMIC_RELEASE);
	if (ca_bitperfect && !__atomic_load_n(&ca_volume_verified, __ATOMIC_ACQUIRE)) { LOG_ERROR("bit-perfect mode cannot verify unity hardware volume"); goto failed; }
	ca_measure_latency();
	ca_add_listeners();
	__atomic_store_n(&output.error_opening, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ca_device_changed, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ca_open_failures, 0U, __ATOMIC_RELEASE);
	if (__atomic_load_n(&ca_fallback_active, __ATOMIC_ACQUIRE))
		LOG_WARN("running in shared fallback mode on device %u: not bit-perfect, hardware volume active", (unsigned)audio_device);
	LOG_INFO("signal path: mode=%s -> S32 %s%s -> %s -> CoreAudio %u Hz -> device %u%s",
		ca_mode, ca_transport(), ca_dither ? " + TPDF24" : "",
		ca_float_client ? "Float32 shared mixer" : "S32 direct", output.current_sample_rate,
		(unsigned)audio_device, ca_bitperfect ? " [bit-perfect]" : "");
	LOG_INFO("opened CoreAudio device %u: requested=%u Hz hardware=%u Hz profile=%s%s%s",
		(unsigned)audio_device, output.current_sample_rate, __atomic_load_n(&device_rate, __ATOMIC_ACQUIRE),
		ca_profile, ca_exclusive ? " exclusive" : "", ca_bitperfect ? " bit-perfect" : "");
	return;
failed:
	ca_dispose();
	if (found && ca_allow_fallback && !__atomic_load_n(&ca_fallback_active, __ATOMIC_ACQUIRE) && (ca_exclusive || ca_bitperfect)) {
		/* the device exists but cannot be hogged or does not offer an integer bit-perfect stream:
		 * playing through the shared mixer beats silence plus a restart loop. 'strict' disables this. */
		LOG_WARN("exclusive/bit-perfect output is not available on this device - falling back to shared Float32 output (add 'strict' to the -a parameters to disable this fallback)");
		__atomic_store_n(&ca_fallback_active, true, __ATOMIC_RELEASE);
		ca_exclusive = false;
		ca_float_client = true;
		ca_bitperfect = false;
		goto retry;
	}
	__atomic_store_n(&output.error_opening, true, __ATOMIC_RELEASE);
	/* a failed open must not leave the flags of its partial progress in the telemetry */
	__atomic_store_n(&ca_physical_verified, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ca_volume_verified, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ca_exclusive_verified, false, __ATOMIC_RELEASE);
	{
		unsigned failures = __atomic_add_fetch(&ca_open_failures, 1U, __ATOMIC_ACQ_REL);
		__atomic_store_n(&ca_open_fail_ms, gettime_ms(), __ATOMIC_RELEASE);
		if (failures == 1) LOG_ERROR("unable to open CoreAudio output");
		else LOG_INFO("unable to open CoreAudio output (attempt %u, next retry in %u s)", failures, ca_retry_delay_ms(failures) / 1000U);
	}
}

void output_init_coreaudio(log_level level, const char *device, unsigned output_buf_size, char *params,
		unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;
	ca_exclusive = ca_param_enabled(params, "exclusive");
	ca_float_client = !ca_exclusive;
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
	char *headroom_end = NULL;
	errno = 0;
	ca_headroom_db = headroom ? strtod(headroom, &headroom_end) : (!strcmp(ca_mode, "pcm-studio") ? 1.0 : 0.0);
	if (errno || !isfinite(ca_headroom_db) || (headroom && (!headroom_end || *headroom_end)) || ca_headroom_db < 0.0 || ca_headroom_db > 12.0) {
		LOG_WARN("invalid headroom %.2f dB; using 0 dB", ca_headroom_db);
		ca_headroom_db = 0.0;
	}
	ca_headroom_gain = to_gain((float)pow(10.0, -ca_headroom_db / 20.0));
	if (!strcmp(ca_mode, "pcm-studio")) ca_dither = true;
	if (ca_bitperfect) {
		if (!ca_exclusive) LOG_INFO("bit-perfect processing requested with shared CoreAudio access");
		ca_dither = false;
		ca_headroom_db = 0.0;
		ca_headroom_gain = FIXED_ONE;
	}
	ca_allow_fallback = !ca_param_enabled(params, "strict");
	ca_req_exclusive = ca_exclusive;
	ca_req_float_client = ca_float_client;
	ca_req_bitperfect = ca_bitperfect;
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
	mach_timebase_info(&ca_timebase);
	__atomic_store_n(&ca_monitor_running, true, __ATOMIC_RELEASE);
	ca_monitor_started = pthread_create(&ca_monitor_thread, NULL, ca_monitor, NULL) == 0;
	if (!ca_monitor_started) {
		__atomic_store_n(&ca_monitor_running, false, __ATOMIC_RELEASE);
		LOG_WARN("CoreAudio recovery monitor unavailable");
	}
	_coreaudio_open();
}

void output_close_coreaudio(void) {
	__atomic_store_n(&ca_monitor_running, false, __ATOMIC_RELEASE);
	if (ca_monitor_started) {
		pthread_join(ca_monitor_thread, NULL);
		ca_monitor_started = false;
	}
	ca_dispose();
	output_close_common();
}

#endif
