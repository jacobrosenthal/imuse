#ifndef LIBIMUSE_INTERNAL_WAVE_RENDERER_H
#define LIBIMUSE_INTERNAL_WAVE_RENDERER_H

/* SPSC audio ring. The game thread commits u8 offset-binary stereo (or port
 * quad FL,FR,BL,BR) halves; the audio thread drains and upsamples them into
 * the float mix bus. Atomic state publishes filled halves and
 * requested filter mode. Underruns produce silence. */

#include <stdint.h>

#include <imuse/handle.h>

#include "internal/mixer.h" /* ImDigitalOutBuf */

/* Half geometry: identical to the SDL wave driver's so the engine
 * tick-vs-mix-frame ratio stays the same and the back-pressure window
 * is preserved. 1024 u8 bytes per half = 512 stereo frames = ~23.2 ms
 * at 22050 Hz stereo (waveSpeed=1) or ~46.4 ms at 11025 Hz. */
#define IM_WAVERENDERER_HALF_FRAMES 512
#define IM_WAVERENDERER_MAX_CHANNELS 4
#define IM_WAVERENDERER_MAX_HALF_BYTES (IM_WAVERENDERER_HALF_FRAMES * IM_WAVERENDERER_MAX_CHANNELS)
#define IM_WAVERENDERER_NUM_HALVES 3
#define IM_WAVERENDERER_FIR_TAPS 127
#define IM_WAVERENDERER_FIR_PAIRS ((IM_WAVERENDERER_FIR_TAPS - 1) / 2)

/* Renderer state (embedded in struct imuse). */
typedef struct ImWaveRendererState {
	/* SPSC ring of u8 offset-binary samples. Producer (game thread)
	 * fills halves[head]; consumer (audio thread) drains halves[tail].
	 * filledHalves publishes completed audio. */
	uint8_t halves[IM_WAVERENDERER_NUM_HALVES][IM_WAVERENDERER_MAX_HALF_BYTES];
	int16_t mixScratch[IM_WAVERENDERER_MAX_HALF_BYTES];

	/* Producer cursors (game thread only). */
	int produceIdx;   /* index into halves[] currently
						 handed out via Acquire */
	int produceArmed; /* 1 between Acquire and Commit */

	/* Consumer cursors (audio thread only). */
	int consumeIdx;    /* halves[] index being drained */
	int consumeOffset; /* bytes already drained from
						  halves[consumeIdx] (0..halfBytes) */

	/* Cross-thread: count of halves committed and not yet fully drained.
	 * Updated atomically. Producer Commit does +1 with release; consumer
	 * decrement uses release after advancing consumeIdx so producer sees
	 * the slot is free before it potentially overwrites. */
	_Atomic int filledHalves;

	/* Cached engine-side config. sampleRateMode mirrors the DOS wave
	 * driver's status block (0 = ~11 kHz, 1 = ~22 kHz). */
	int sampleRateMode;
	int channels;  /* 2 = stereo, 4 = quad FL,FR,BL,BR */
	int halfBytes; /* HALF_FRAMES * channels */

	/* Rate plumbing for the audio-thread upsample step. The engine
	 * always produces samples at waveSampleRate (22050 Hz with
	 * waveSpeed=1, 11025 Hz with waveSpeed=0); imuse_mix consumes
	 * at outputSampleRate (typically 44100 to match the MIDI mix bus).
	 * PullSamples expands the source by
	 * `upsampleRatio` via zero-order hold. The optional SB16 FIR then
	 * removes the spectral images. Fractional ratios are clamped to
	 * an integer with a one-shot debug warning. */
	int waveSampleRate;
	int outputSampleRate;
	int upsampleRatio;
	float held[IM_WAVERENDERER_MAX_CHANNELS];
	int repeatRemaining;

	/* Optional SB16 reconstruction filter. Coefficients are symmetric,
	 * so each pair multiplies the sum of two history samples. Near-zero
	 * half-band coefficients are omitted from the compact pair arrays. */
	_Atomic int requestedFilter;
	int waveFilterEnabled;
	float firHistory[IM_WAVERENDERER_FIR_TAPS][IM_WAVERENDERER_MAX_CHANNELS];
	float firPairCoefficients[IM_WAVERENDERER_FIR_PAIRS];
	uint8_t firPairTaps[IM_WAVERENDERER_FIR_PAIRS];
	int firPairCount;
	float firCenterCoefficient;
	int firHistoryPos;

	/* Pre-built output descriptor handed back via Acquire. The buffer
	 * pointer is rebound on each call to halves[produceIdx]. */
	ImDigitalOutBuf outDesc;

	/* Initialisation flag — guards against Acquire/Pull races during
	 * the create/destroy windows. */
	int initialized;
} ImWaveRendererState;

/* ===== Lifecycle ===== */

int ImWaveRenderer_Init(imuse_t* im);
int ImWaveRenderer_Deinit(imuse_t* im);

/* ===== Producer side (game thread) ===== */

/* Hand out the next free half as an ImDigitalOutBuf. The descriptor's
 * `buffer` pointer aliases halves[produceIdx], so when ImMixer_audioWriteToDriver
 * compands the int16 mix into outDesc->buffer it writes straight into
 * the renderer's storage. *mixBufferOut is set to the int16 mix scratch
 * (the mixer will fill it). Returns NULL when the ring is full (back-
 * pressure: caller skips this tick). */
ImDigitalOutBuf* ImWaveRenderer_AcquireProduceFrame(imuse_t* im, int16_t** mixBufferOut);

/* Publish the previously-acquired half to the consumer and advance
 * the producer head. The half's u8 contents must have been written
 * before this call (typically via ImMixer_audioWriteToDriver). */
void ImWaveRenderer_CommitProduceFrame(imuse_t* im);

/* ===== Consumer side (audio thread) ===== */

/* Pull `frames` frames worth of samples from the ring, convert u8
 * offset-binary to float in [-1, +1], and add them on top of `dst`
 * (interleaved, dstChannels = 2 or 4). A quad ring pulled into stereo
 * folds the rear pair into the front at -3 dB; a stereo ring pulled into
 * quad adds only to the front pair. On underrun, the missing frames
 * produce no contribution (caller's existing buffer content is preserved).
 *
 * Safe to call concurrently with the producer (SPSC). */
void ImWaveRenderer_PullSamples(imuse_t* im, float* dst, int frames, int dstChannels);

#endif /* LIBIMUSE_INTERNAL_WAVE_RENDERER_H */
