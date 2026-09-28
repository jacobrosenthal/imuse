#ifndef LIBIMUSE_INTERNAL_MIDI_BACKEND_H
#define LIBIMUSE_INTERNAL_MIDI_BACKEND_H

/*
 * libimuse — MIDI backend vtable + engine-side dispatch.
 *
 * The MIDI synth is a per-session pluggable component. Built-in backends
 * currently cover FluidSynth, Nuked SC-55, and the TIE FM4 OPL3 driver. The
 * engine's slots / mix code only references the typed dispatch helpers below;
 * backends register a
 * vtable matching this struct and the dispatcher fans calls through
 * im->midi.backend.
 *
 * Adding a new backend means writing one .c file with a vtable
 * instance + factory; no changes to slots.c, mix.c, or this header.
 *
 * Threading: open / close / per-event ops run on the game thread;
 * render_float runs on the audio thread. Backend implementations
 * are responsible for whatever synchronization that requires.
 */

#include <imuse/commands.h> /* ImuseLogFunc */
#include <imuse/handle.h>
#include <imuse/midi_backend.h>

struct ImuseMidiBackend {
	/* ===== Lifecycle ===== */

	/* Bring up the synth with the supplied output sample rate and
	 * host log hooks. Called by ImCommands_Init before the first
	 * event is emitted. The backend should remember logFunc/logUser
	 * if it needs to log after open returns (typically only error
	 * paths and trace events). Returns 0 on success, negative on
	 * failure (which aborts imuse_create). */
	int (*open)(ImuseMidiBackend* self, int sampleRate, ImuseLogFunc logFunc, void* logUser);

	/* Tear down the synth. Called by ImCommands_Terminate. After
	 * close returns, no event ops will be invoked until release.
	 * Returns 0 on success. */
	int (*close)(ImuseMidiBackend* self);

	/* Free the backend struct + any state. Called by
	 * imuse_midi_backend_release (host-driven). The handle must
	 * not be used after this returns. */
	void (*release)(ImuseMidiBackend* self);

	/* ===== Per-event ops (game thread) =====
	 *
	 * Backends queue semantic events or generated register writes for rendering.
	 * The host only dispatches events between open() and close(). */
	void (*program_change)(ImuseMidiBackend* self, int channel, int program);
	void (*note_on)(ImuseMidiBackend* self, int channel, int note, int velocity);
	void (*note_off)(ImuseMidiBackend* self, int channel, int note);
	void (*control_change)(ImuseMidiBackend* self, int channel, unsigned int ctrl, int value);
	void (*pitch_bend)(ImuseMidiBackend* self, int channel, int wire14bit);

	/* ===== Audio render (audio thread) =====
	 *
	 * Render `frames` stereo float frames into `buf` (interleaved
	 * L,R,L,R,...). Overwrites buf. NULL-safe (writes silence). */
	void (*render_float)(ImuseMidiBackend* self, float* buf, int frames);
	/* Payload is copied before returning; ordered with channel events. */
	void (*instrument)(ImuseMidiBackend* self, int channel, const uint8_t* data, size_t size);
	void (*sysex)(ImuseMidiBackend* self, const uint8_t* data, size_t size);
	/* X-Wing Roland memory is indexed by logical channel, independently of the
	 * eight physical MT-32 parts. All hooks enqueue copied, ordered events. */
	void (*roland_program)(ImuseMidiBackend* self, int slot, int program);
	void (*roland_instrument)(ImuseMidiBackend* self, int slot, const uint8_t* data, size_t size);
	void (*roland_select)(ImuseMidiBackend* self, int channel, int slot);
	void (*roland_edit)(ImuseMidiBackend* self, int slot, int channel, int offset, const uint8_t* data,
						size_t size);
	void (*roland_bend)(ImuseMidiBackend* self, int channel, int detune);
	/* DOS93 AdLib driver state belongs to the sequencer thread; only copied
	 * register writes cross to rendering. Pitch is signed, 128 units/semitone. */
	void (*adlib_service)(ImuseMidiBackend* self);
	void (*adlib_bend)(ImuseMidiBackend* self, int channel, int detune);
	void (*adlib_notes)(ImuseMidiBackend* self, unsigned int channels, unsigned int notes[128]);
	int melodicChannels; /* zero = standard 15; MT-32 = 8; AdLib = 32, no drum gap */
};

/* ===== Engine-side dispatch =====
 *
 * NULL-safe wrappers that fan out to im->midi.backend. The slots /
 * mix modules only call these — they don't see the vtable. */
void ImMidi_ProgramChange(imuse_t* im, int channel, int program);
void ImMidi_NoteOn(imuse_t* im, int channel, int note, int velocity);
void ImMidi_NoteOff(imuse_t* im, int channel, int note);
void ImMidi_ControlChange(imuse_t* im, int channel, unsigned int ctrl, int value);
void ImMidi_PitchBend(imuse_t* im, int channel, int wire14bit);
void ImMidi_RenderFloat(imuse_t* im, float* buf, int frames);

/* Release a backend handle via its release() vtable slot. Internal
 * helper used by imuse_create rollback and imuse_destroy; not
 * exposed to the host (lifetime is library-owned). NULL-safe. */
void ImMidi_BackendRelease(ImuseMidiBackend* backend);

#endif /* LIBIMUSE_INTERNAL_MIDI_BACKEND_H */
