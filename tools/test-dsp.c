#include "../squeezelite.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

log_level loglevel = lERROR;

const char *logtime(void) { return "test"; }
void logprint(const char *format, ...) {
	(void)format;
}

static void test_preamp_and_telemetry(void) {
	s32_t samples[] = { 1073741824, -1073741824 };
	struct dsp_telemetry telemetry;
	assert(dsp_init("preamp=-6;headroom=0"));
	assert(dsp_newstream(44100));
	dsp_process(samples, 1);
	assert(labs((long)samples[0] - 538145694L) < 4);
	assert(labs((long)samples[1] + 538145694L) < 4);
	dsp_get_telemetry(&telemetry);
	assert(telemetry.frames == 1);
	assert(telemetry.clipped_samples == 0);
	assert(telemetry.peak_dbfs < -11.99 && telemetry.peak_dbfs > -12.03);
}

static void test_channel_isolation_and_flush(void) {
	s32_t samples[64] = { 0 };
	unsigned i;
	samples[0] = 1073741824;
	assert(dsp_init("peak=1000:0.707:6;headroom=6"));
	assert(dsp_newstream(48000));
	dsp_process(samples, 32);
	for (i = 0; i < 32; ++i) assert(samples[i * 2 + 1] == 0);
	dsp_flush();
}

static void test_clipping_and_validation(void) {
	s32_t samples[] = { INT32_MAX, INT32_MIN };
	struct dsp_telemetry telemetry;
	assert(dsp_init("preamp=6;headroom=0"));
	assert(dsp_newstream(44100));
	dsp_process(samples, 1);
	dsp_get_telemetry(&telemetry);
	assert(samples[0] == INT32_MAX && samples[1] == INT32_MIN);
	assert(telemetry.clipped_samples == 2);
	assert(!dsp_init("unknown=1"));
	assert(dsp_init("peak=30000:0.7:3;headroom=3"));
	assert(!dsp_newstream(44100));
}

static void test_bypass_null(void) {
	s32_t samples[512], original[512]; unsigned i;
	for (i = 0; i < 512; ++i) samples[i] = (s32_t)(i * 7919 - 1000000);
	memcpy(original, samples, sizeof(samples));
	assert(dsp_init("preamp=12;headroom=0;bypass=true"));
	assert(dsp_newstream(48000)); dsp_process(samples, 256);
	assert(!memcmp(samples, original, sizeof(samples)));
}

static void test_impulse_and_frequency_response(void) {
	s32_t impulse[2048] = { 0 }; struct dsp_telemetry telemetry; unsigned i; bool tail = false;
	impulse[0] = impulse[1] = 1073741824;
	assert(dsp_init("preamp=0;headroom=auto;eq=6,0,0,0,0,0,0,0,0,0,0,0"));
	assert(dsp_newstream(48000)); dsp_process(impulse, 1024);
	for (i = 2; i < 2048; i += 2) if (impulse[i]) { tail = true; break; }
	assert(tail);
	dsp_get_telemetry(&telemetry);
	/* Combined-response analysis should reserve roughly the measured 31 Hz boost. */
	assert(telemetry.response_peak_db > 5.8 && telemetry.response_peak_db < 6.3);
	assert(fabs(telemetry.applied_gain_db + telemetry.response_peak_db) < 0.01);
}

static void test_click_free_swap(void) {
	s32_t block[1024]; struct dsp_telemetry telemetry; unsigned i; s32_t previous = 0; long long worst = 0;
	assert(dsp_init("preamp=0;headroom=0")); assert(dsp_newstream(48000));
	for (i = 0; i < 1024; ++i) block[i] = 536870912;
	dsp_process(block, 512);
	assert(dsp_configure("preamp=-24;headroom=0"));
	for (i = 0; i < 1024; ++i) block[i] = 536870912;
	dsp_process(block, 512);
	for (i = 0; i < 1024; i += 2) { long long delta = llabs((long long)block[i] - previous); if (i && delta > worst) worst = delta; previous = block[i]; }
	/* A hard switch is ~503M counts; the 256-frame ramp is below 3M per frame. */
	assert(worst < 3000000);
	dsp_get_telemetry(&telemetry); assert(telemetry.config_swaps == 1);
}

static void test_json_persistence_and_rollback(void) {
	const char *path = "/tmp/apple-squeezer-dsp-test.json";
	const char *json1 = "{\"version\":1,\"player_id\":\"test-player\",\"bypass\":false,\"preamp_db\":-1.5,\"headroom_db\":null,\"graphic_eq_db\":[0,0,0,0,0,0,0,0,0,0,0,0],\"parametric\":\"peak=1000:1:-2\"}";
	const char *json2 = "{\"version\":1,\"player_id\":\"test-player\",\"bypass\":true,\"preamp_db\":0,\"headroom_db\":0,\"graphic_eq_db\":[0,0,0,0,0,0,0,0,0,0,0,0],\"parametric\":\"\"}";
	const char *json3 = "{\"version\":2,\"player_id\":\"test-player\",\"bypass\":false,\"preamp_db\":0,\"headroom_db\":null,\"graphic_eq_db\":[0,0,0,0,0,0,0,0,0,0,0,0],\"graphic_eq_enabled\":[true,true,true,true,true,true,true,true,true,true,true,true],\"parametric\":\"\",\"fir_file\":\"\",\"fir_normalize\":true}";
	char *options;
	unlink(path); unlink("/tmp/apple-squeezer-dsp-test.json.bak"); unlink("/tmp/apple-squeezer-dsp-test.json.rejected");
	assert(dsp_config_save(path, "test-player", json1));
	options = dsp_config_load_options(path, "test-player"); assert(options && strstr(options, "preamp=-1.5")); free(options);
	assert(!dsp_config_load_options(path, "wrong-player"));
	assert(dsp_config_save(path, "test-player", json2));
	assert(dsp_config_rollback(path));
	options = dsp_config_load_options(path, "test-player"); assert(options && strstr(options, "preamp=-1.5")); free(options);
	assert(dsp_config_save(path, "test-player", json3));
	options = dsp_config_load_options(path, "test-player"); assert(options && !strstr(options, "fir=;")); free(options);
	unlink(path); unlink("/tmp/apple-squeezer-dsp-test.json.rejected");
}

static void test_repeated_stream_and_low_rate_flat_eq(void) {
	s32_t samples[] = { 123456789, -987654321 };
	assert(dsp_init("preamp=0;headroom=0;eq=0,0,0,0,0,0,0,0,0,0,0,0"));
	assert(dsp_newstream(32000)); dsp_process(samples, 1); dsp_flush();
	assert(dsp_newstream(44100)); dsp_process(samples, 1);
}

static void write_test_ir(const char *path) {
	/* 48 kHz mono PCM16: delayed unit impulse at frame 3. */
	unsigned char wav[52] = {
		'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
		1,0,1,0,0x80,0xbb,0,0,0,0x77,1,0,2,0,16,0,'d','a','t','a',8,0,0,0,
		0,0,0,0,0,0,0xff,0x7f
	};
	FILE *file = fopen(path, "wb"); assert(file); assert(fwrite(wav, 1, sizeof(wav), file) == sizeof(wav)); fclose(file);
}

static void write_partitioned_ir(const char *path, unsigned frames) {
	unsigned char header[44] = {
		'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
		1,0,2,0,0x80,0xbb,0,0,0,0xee,2,0,4,0,16,0,'d','a','t','a',0,0,0,0
	};
	unsigned bytes = frames * 4, riff = bytes + 36, i;
	FILE *file;
	header[4]=riff; header[5]=riff>>8; header[6]=riff>>16; header[7]=riff>>24;
	header[40]=bytes; header[41]=bytes>>8; header[42]=bytes>>16; header[43]=bytes>>24;
	file=fopen(path,"wb"); assert(file); assert(fwrite(header,1,sizeof(header),file)==sizeof(header));
	for(i=0;i<frames;i++){
		short left=i==0?32767:(i==1?16384:0),right=i==2?32767:0;
		assert(fwrite(&left,sizeof(left),1,file)==1); assert(fwrite(&right,sizeof(right),1,file)==1);
	}
	fclose(file);
}

static void test_partitioned_fir_impulse_and_guard(void) {
	const char *ir="/tmp/apple-squeezer-partitioned-ir.wav";
	s32_t samples[1400]={0}; struct dsp_telemetry telemetry; char options[512]; unsigned i;
	write_partitioned_ir(ir,4096);
	snprintf(options,sizeof(options),"fir=%s;fir_normalize=true;fir_max_taps=8192;headroom=0",ir);
	assert(dsp_init(options)); assert(dsp_newstream(48000)); samples[0]=536870912; dsp_process(samples,700);
	dsp_get_telemetry(&telemetry); assert(telemetry.fir_taps==4096); assert(telemetry.fir_partitions==8); assert(telemetry.fir_checksum!=0); assert(telemetry.latency_frames==512);
	assert(labs((long)samples[1024]-536870912L)<32768); assert(labs((long)samples[1026]-268443648L)<32768);
	for(i=0;i<700;i++)assert(samples[i*2+1]==0);
	dsp_flush();
	snprintf(options,sizeof(options),"fir=%s;fir_max_taps=2048;headroom=0",ir);
	assert(dsp_init(options)); assert(!dsp_newstream(48000));
	unlink(ir);
}

static void test_fir_trim_mapping_and_latency_reference(void) {
	const char *ir="/tmp/apple-squeezer-fir-controls.wav";
	s32_t samples[1200]={0}; struct dsp_telemetry telemetry; char options[512];
	write_partitioned_ir(ir,4096);
	snprintf(options,sizeof(options),"fir=%s;fir_trim=-80;fir_channel_map=swap;fir_latency=center;headroom=0",ir);
	assert(dsp_init(options)); assert(dsp_newstream(48000)); samples[0]=samples[1]=536870912; dsp_process(samples,600);
	dsp_get_telemetry(&telemetry); assert(telemetry.fir_taps==3); assert(telemetry.fir_partitions==1); assert(telemetry.latency_frames==513);
	assert(labs((long)samples[1024]-0L)<32768); assert(labs((long)samples[1025]-536870912L)<32768);
	assert(labs((long)samples[1027]-268443648L)<32768); assert(labs((long)samples[1028]-536870912L)<32768);
	dsp_flush();
	assert(!dsp_init("preamp=0;fir_channel_map=invalid"));
	assert(!dsp_init("preamp=0;fir_latency=invalid"));
	assert(!dsp_init("preamp=0;fir_trim=-10"));
	unlink(ir);
}

static void test_v2_spatial_true_peak_and_fir(void) {
	const char *ir = "/tmp/apple-squeezer-test-ir.wav";
	const char *path = "/tmp/apple-squeezer-dsp-v2.json";
	char json[2048], *options, response[8192];
	s32_t samples[64] = { 0 }; struct dsp_telemetry telemetry;
	write_test_ir(ir);
	snprintf(json, sizeof(json), "{\"version\":2,\"player_id\":\"test-player\",\"bypass\":false,\"preamp_db\":0,\"headroom_db\":null,\"graphic_eq_db\":[0,0,0,0,0,0,0,0,0,0,0,0],\"parametric\":\"peak=1000:1:2:off\",\"fir_file\":\"%s\",\"fir_gain_db\":0,\"balance\":0,\"stereo_width\":1.2,\"mono\":false,\"polarity\":\"right\",\"delay_left_ms\":0.5,\"delay_right_ms\":0,\"crossfeed\":\"light\",\"loudness_db\":2,\"true_peak\":true,\"limiter\":true,\"limiter_ceiling_db\":-1,\"replaygain_db\":1,\"replaygain_headroom\":true}", ir);
	unlink(path); assert(dsp_config_save(path, "test-player", json));
	options = dsp_config_load_options(path, "test-player"); assert(options && strstr(options, "fir=") && strstr(options, "crossfeed=light"));
	assert(dsp_init(options)); free(options); assert(dsp_newstream(48000));
	samples[0] = samples[1] = 1073741824; dsp_process(samples, 32); dsp_get_telemetry(&telemetry);
	assert(telemetry.frames == 32 && telemetry.fir_active && telemetry.fir_taps == 4);
	assert(telemetry.latency_frames >= 24 && telemetry.limiter_active);
	assert(dsp_response_json(response, sizeof(response), 32) && strstr(response, "\"phase\""));
	unlink(path); unlink(ir);
}

static void test_linked_limiter_and_dynamic_loudness(void) {
	s32_t samples[2048];
	struct dsp_telemetry telemetry;
	unsigned i;
	assert(dsp_init("preamp=12;headroom=0;true_peak=true;limiter=true;limiter_ceiling=-1;loudness=10"));
	dsp_set_volume(FIXED_ONE / 2, FIXED_ONE / 2);
	assert(dsp_newstream(48000));
	for (i = 0; i < 1024; ++i) samples[i * 2] = samples[i * 2 + 1] = 1073741824;
	dsp_process(samples, 1024);
	dsp_get_telemetry(&telemetry);
	assert(telemetry.latency_frames >= 240);
	assert(telemetry.limiter_events > 0);
	assert(telemetry.limiter_gain_reduction_db > 6);
	assert(telemetry.loudness_compensation_db > 1.4 && telemetry.loudness_compensation_db < 1.6);
	/* Linked limiting must preserve identical stereo samples. */
	for (i = 240; i < 1024; ++i) assert(samples[i * 2] == samples[i * 2 + 1]);
}

int main(void) {
	test_preamp_and_telemetry();
	test_channel_isolation_and_flush();
	test_clipping_and_validation();
	test_bypass_null();
	test_impulse_and_frequency_response();
	test_click_free_swap();
	test_json_persistence_and_rollback();
	test_repeated_stream_and_low_rate_flat_eq();
	test_v2_spatial_true_peak_and_fir();
	test_partitioned_fir_impulse_and_guard();
	test_fir_trim_mapping_and_latency_reference();
	test_linked_limiter_and_dynamic_loudness();
	puts("native DSP tests passed");
	return 0;
}
