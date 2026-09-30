#include <imuse/commands.h>

#include "internal/midi_backend.h"
#include "internal/wave_renderer.h"

#include <stdint.h>
#include <string.h>

/* MIDI writes an interleaved stereo float bus and the wave renderer mixes into
 * it. The s16 entry point processes that bus in chunks and saturates on output.
 * Concurrency is handled by the wave SPSC ring and the selected MIDI backend. */

/* Bound audio-callback stack use independently of the requested frame count. */
#define IMUSE_MIX_S16_CHUNK_FRAMES 512
#define IMUSE_MIX_MAX_CHANNELS 4

/* MIDI backends render interleaved stereo. Quad output renders them into a
 * scratch pair and places them on the front channels. */
static void mix_render_midi(imuse_t* im, float* buf, int frames, int channels) {
	if (channels == 2) {
		ImMidi_RenderFloat(im, buf, frames);
		return;
	}
	float stereo[IMUSE_MIX_S16_CHUNK_FRAMES * 2];
	int done = 0;
	while (done < frames) {
		int chunk = frames - done;
		if (chunk > IMUSE_MIX_S16_CHUNK_FRAMES)
			chunk = IMUSE_MIX_S16_CHUNK_FRAMES;
		ImMidi_RenderFloat(im, stereo, chunk);
		float* out = &buf[(size_t)done * (size_t)channels];
		for (int frame = 0; frame < chunk; ++frame) {
			out[0] = stereo[2 * frame];
			out[1] = stereo[2 * frame + 1];
			for (int channel = 2; channel < channels; ++channel)
				out[channel] = 0.0f;
			out += channels;
		}
		done += chunk;
	}
}

int imuse_mix_f32_channels(imuse_t* im, float* buf, int frames, int channels) {
	if (channels != 2 && channels != 4)
		return -1;
	if (!buf || frames <= 0)
		return 0;
	if (!im) {
		memset(buf, 0, (size_t)frames * (size_t)channels * sizeof(float));
		return 0;
	}
	mix_render_midi(im, buf, frames, channels);
	ImWaveRenderer_PullSamples(im, buf, frames, channels);
	return 0;
}

int imuse_mix_s16_channels(imuse_t* im, int16_t* buf, int frames, int channels) {
	if (channels != 2 && channels != 4)
		return -1;
	if (!buf || frames <= 0)
		return 0;
	if (!im) {
		memset(buf, 0, (size_t)frames * (size_t)channels * sizeof(int16_t));
		return 0;
	}

	float scratch[IMUSE_MIX_S16_CHUNK_FRAMES * IMUSE_MIX_MAX_CHANNELS];
	int written = 0;
	while (written < frames) {
		int chunk = frames - written;
		if (chunk > IMUSE_MIX_S16_CHUNK_FRAMES)
			chunk = IMUSE_MIX_S16_CHUNK_FRAMES;

		/* Float bus: GMIDI overwrites, wave adds. Identical math to
		 * imuse_mix_f32_channels except into the local scratch. */
		mix_render_midi(im, scratch, chunk, channels);
		ImWaveRenderer_PullSamples(im, scratch, chunk, channels);

		/* Saturating cast. Wave contribution is bounded ±1 by the
		 * companding LUT; the synthesized contribution can still make the sum
		 * exceed ±1 in dense passages — clamp before the integer cast or sign-flip
		 * wraparound produces a click. */
		int16_t* dst = &buf[(size_t)written * (size_t)channels];
		int n = chunk * channels;
		for (int i = 0; i < n; ++i) {
			float x = scratch[i];
			if (x > 1.0f)
				x = 1.0f;
			if (x < -1.0f)
				x = -1.0f;
			dst[i] = (int16_t)(x * 32767.0f);
		}

		written += chunk;
	}
	return 0;
}

void imuse_mix_f32(imuse_t* im, float* buf, int frames) { (void)imuse_mix_f32_channels(im, buf, frames, 2); }

void imuse_mix_s16(imuse_t* im, int16_t* buf, int frames) {
	(void)imuse_mix_s16_channels(im, buf, frames, 2);
}
