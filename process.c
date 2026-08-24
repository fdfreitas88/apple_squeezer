/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2026, ralph_irving@hotmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// sample processing - only included when building with PROCESS set

#include "squeezelite.h"

#if PROCESS

extern log_level loglevel;

extern struct buffer *outputbuf;
extern struct decodestate decode;
struct processstate process;
extern struct codec *codec;
#if DSD
extern struct outputstate output;
#endif

#define LOCK_D   mutex_lock(decode.mutex);
#define UNLOCK_D mutex_unlock(decode.mutex);
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)

#if RESAMPLE
static bool resample_enabled;
static bool resample_active;
#endif
#if DSP
static bool native_dsp_enabled;
static bool native_dsp_active;
static u64_t native_dsp_last_report;

static void _report_native_dsp(void) {
	struct dsp_telemetry telemetry;
	dsp_get_telemetry(&telemetry);
	LOG_INFO("native DSP track: frames=%llu peak=%.2fdBFS true_peak=%.2fdBTP true_peak_overs=%llu clipped=%llu gain=%.2fdB replaygain=%.2fdB loudness=%.2fdB response_peak=%.2fdB limiter_reduction=%.2fdB limiter_events=%llu swaps=%llu latency=%u fir_taps=%u fir_partitions=%u fir_checksum=%016llx limiter=%s",
		(unsigned long long)telemetry.frames, telemetry.peak_dbfs, telemetry.true_peak_dbfs,
		(unsigned long long)telemetry.true_peak_overs,
		(unsigned long long)telemetry.clipped_samples, telemetry.applied_gain_db,
		telemetry.replaygain_db, telemetry.loudness_compensation_db, telemetry.response_peak_db,
		telemetry.limiter_gain_reduction_db, (unsigned long long)telemetry.limiter_events,
		(unsigned long long)telemetry.config_swaps, telemetry.latency_frames,
		telemetry.fir_taps, telemetry.fir_partitions, (unsigned long long)telemetry.fir_checksum,
		telemetry.limiter_active ? "on" : "off");
}
#endif

static void _run_samples(void) {
#if RESAMPLE
	if (resample_active) {
		resample_samples(&process);
	} else
#endif
	{
		process.out_frames = process.in_frames;
		memcpy(process.outbuf, process.inbuf, process.in_frames * BYTES_PER_FRAME);
		process.total_in += process.in_frames;
		process.total_out += process.out_frames;
	}
#if DSP
	if (native_dsp_active && process.out_frames) {
		dsp_process((s32_t *)process.outbuf, process.out_frames);
		if (process.out_sample_rate && process.total_out - native_dsp_last_report >= process.out_sample_rate) {
			native_dsp_last_report = process.total_out;
			_report_native_dsp();
		}
	}
#endif
}


// transfer all processed frames to the output buf
static void _write_samples(void) {
	frames_t frames = process.out_frames;
	u32_t *iptr   = (u32_t *)process.outbuf;
	unsigned cnt  = 10;

	LOCK_O;

	while (frames > 0) {

		frames_t f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
		u32_t *optr = (u32_t *)outputbuf->writep;

		if (f > 0) {

			f = min(f, frames);
			
			memcpy(optr, iptr, f * BYTES_PER_FRAME);
			
			frames -= f;
			
			_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
			iptr += f * BYTES_PER_FRAME / sizeof(*iptr);

		} else if (cnt--) {

			// there should normally be space in the output buffer, but may need to wait during drain phase
			UNLOCK_O;
			usleep(10000);
			LOCK_O;

		} else {

			// bail out if no space found after 100ms to avoid locking
			LOG_ERROR("unable to get space in output buffer");
			UNLOCK_O;
			return;
		}
	}

	UNLOCK_O;
}

// process samples - called with decode mutex set
void process_samples(void) {

	_run_samples();

	_write_samples();

	process.in_frames = 0;
}

// drain at end of track - called with decode mutex set
void process_drain(void) {
	bool done;

	do {

#if RESAMPLE
		if (resample_active) {
			done = resample_drain(&process);
#if DSP
			if (native_dsp_active && process.out_frames) dsp_process((s32_t *)process.outbuf, process.out_frames);
#endif
		} else
#endif
		{
			process.out_frames = 0;
			done = true;
		}

		_write_samples();

	} while (!done);

	LOG_DEBUG("processing track complete - frames in: %lu out: %lu", process.total_in, process.total_out);
#if DSP
	if (native_dsp_active) {
		_report_native_dsp();
	}
#endif
}	

// new stream - called with decode mutex set
unsigned process_newstream(bool *direct, unsigned raw_sample_rate, unsigned supported_rates[]) {

	bool active = false;
	bool pcm_stream = true;
#if DSD
	pcm_stream = output.next_fmt == PCM;
	if (!pcm_stream) LOG_INFO("DSD/DoP stream bypasses PCM resampling and native DSP");
#endif

#if RESAMPLE
	resample_active = pcm_stream && resample_enabled && resample_newstream(&process, raw_sample_rate, supported_rates);
	active = resample_active;
	if (!resample_active) process.in_sample_rate = process.out_sample_rate = raw_sample_rate;
#else
	process.in_sample_rate = process.out_sample_rate = raw_sample_rate;
#endif
#if DSP
	if (native_dsp_enabled && pcm_stream) {
		unsigned dsp_rate = resample_active ? process.out_sample_rate : raw_sample_rate;
		native_dsp_active = dsp_newstream(dsp_rate);
		active = active || native_dsp_active;
	}
#endif

	LOG_INFO("processing: %s", active ? "active" : "inactive");

	*direct = !active;

	if (active) {

		unsigned max_in_frames, max_out_frames;

		process.in_frames = process.out_frames = 0;
		process.total_in = process.total_out = 0;
#if DSP
		native_dsp_last_report = 0;
#endif

		max_in_frames = codec->min_space / BYTES_PER_FRAME ;

		// increase size of output buffer by 10% as output rate is not an exact multiple of input rate
		if (process.out_sample_rate % process.in_sample_rate == 0) {
			max_out_frames = max_in_frames * (process.out_sample_rate / process.in_sample_rate);
		} else {
			max_out_frames = (int)(1.1 * (float)max_in_frames * (float)process.out_sample_rate / (float)process.in_sample_rate);
		}

		if (process.max_in_frames != max_in_frames) {
			LOG_DEBUG("creating process buf in frames: %u", max_in_frames);
			if (process.inbuf) free(process.inbuf);
			process.inbuf = malloc(max_in_frames * BYTES_PER_FRAME);
			process.max_in_frames = max_in_frames;
		}
		
		if (process.max_out_frames != max_out_frames) {
			LOG_DEBUG("creating process buf out frames: %u", max_out_frames);
			if (process.outbuf) free(process.outbuf);
			process.outbuf = malloc(max_out_frames * BYTES_PER_FRAME);
			process.max_out_frames = max_out_frames;
		}
		
		if (!process.inbuf || !process.outbuf) {
			LOG_ERROR("malloc fail creating process buffers");
			*direct = true;
			return raw_sample_rate;
		}
		
		return process.out_sample_rate;
	}

	return raw_sample_rate;
}

// process flush - called with decode mutex set
void process_flush(void) {

	LOG_INFO("process flush");

#if RESAMPLE
	if (resample_enabled) resample_flush();
	resample_active = false;
#endif
#if DSP
	if (native_dsp_active) dsp_flush();
	native_dsp_active = false;
#endif

	process.in_frames = 0;
}

// init - called with no mutex
void process_init(char *resample_opt, char *dsp_opt) {

	bool enabled = false;

#if RESAMPLE
	resample_enabled = resample_opt && resample_init(resample_opt);
	enabled = resample_enabled;
#endif
#if DSP
	native_dsp_enabled = dsp_opt && dsp_init(dsp_opt);
	enabled = enabled || native_dsp_enabled;
#endif

	memset(&process, 0, sizeof(process));

	if (enabled) {
		LOCK_D;
		decode.process = true;
		UNLOCK_D;
	}
}

#endif // #if PROCESS
