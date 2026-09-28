#ifndef IMUSE_SYNTH_QUEUE_H
#define IMUSE_SYNTH_QUEUE_H
#include "internal/midi_backend.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

/* One game-thread producer and one render-thread consumer. Payloads are copied;
 * native AdLib queues generated register writes instead of semantic events. */
enum { IM_SYNTH_QUEUE_SIZE = 4096 };

enum {
	SYNTH_PROGRAM,
	SYNTH_ON,
	SYNTH_OFF,
	SYNTH_CONTROL,
	SYNTH_BEND,
	SYNTH_INSTRUMENT,
	SYNTH_SYSEX,
	SYNTH_ROLAND_PROGRAM,
	SYNTH_ROLAND_INSTRUMENT,
	SYNTH_ROLAND_SELECT,
	SYNTH_ROLAND_EDIT,
	SYNTH_OPL_WRITE
};

typedef struct ImSynthEvent {
	uint16_t size, value;
	uint8_t type, channel, a, b, data[256];
} ImSynthEvent;

typedef struct ImSynthQueue {
	ImuseMidiBackend api;
	ImSynthEvent events[IM_SYNTH_QUEUE_SIZE];
	atomic_uint read, write;
	atomic_bool overflow;
	ImuseLogFunc log;
	void* logUser;
	int rate;
} ImSynthQueue;

static void synth_enqueue(ImuseMidiBackend* api, ImSynthEvent* e) {
	ImSynthQueue* q = (ImSynthQueue*)api;
	if (!q->rate || atomic_load_explicit(&q->overflow, memory_order_acquire))
		return;
	unsigned int w = atomic_load_explicit(&q->write, memory_order_relaxed);
	unsigned int r = atomic_load_explicit(&q->read, memory_order_acquire);
	if (w - r == IM_SYNTH_QUEUE_SIZE) {
		atomic_store_explicit(&q->overflow, true, memory_order_release);
		if (q->log)
			q->log(q->logUser, IMUSE_LOG_ERROR, "MIDI event queue overflow; output stopped");
		return;
	}
	q->events[w & (IM_SYNTH_QUEUE_SIZE - 1)] = *e;
	atomic_store_explicit(&q->write, w + 1, memory_order_release);
}

static int synth_pop(ImSynthQueue* q, ImSynthEvent* e) {
	unsigned int r = atomic_load_explicit(&q->read, memory_order_relaxed);
	if (r == atomic_load_explicit(&q->write, memory_order_acquire))
		return 0;
	*e = q->events[r & (IM_SYNTH_QUEUE_SIZE - 1)];
	atomic_store_explicit(&q->read, r + 1, memory_order_release);
	return 1;
}

static void synth_program(ImuseMidiBackend* q, int c, int p) {
	ImSynthEvent e = { .type = SYNTH_PROGRAM, .channel = c, .a = p };
	synth_enqueue(q, &e);
}

static void synth_on(ImuseMidiBackend* q, int c, int n, int v) {
	ImSynthEvent e = { .type = SYNTH_ON, .channel = c, .a = n, .b = v };
	synth_enqueue(q, &e);
}

static void synth_off(ImuseMidiBackend* q, int c, int n) {
	ImSynthEvent e = { .type = SYNTH_OFF, .channel = c, .a = n };
	synth_enqueue(q, &e);
}

static void synth_control(ImuseMidiBackend* q, int c, unsigned int n, int v) {
	ImSynthEvent e = { .type = SYNTH_CONTROL, .channel = c, .a = n, .b = v };
	synth_enqueue(q, &e);
}

static void synth_bend(ImuseMidiBackend* q, int c, int v) {
	ImSynthEvent e = { .type = SYNTH_BEND, .channel = c, .value = v };
	synth_enqueue(q, &e);
}

static void synth_instrument(ImuseMidiBackend* q, int c, const uint8_t* d, size_t size) {
	if (size > 256)
		return;
	ImSynthEvent e = { .type = SYNTH_INSTRUMENT, .channel = c, .size = size };
	memcpy(e.data, d, size);
	synth_enqueue(q, &e);
}

static void synth_sysex(ImuseMidiBackend* q, const uint8_t* d, size_t size) {
	if (size > 256)
		return;
	ImSynthEvent e = { .type = SYNTH_SYSEX, .size = size };
	memcpy(e.data, d, size);
	synth_enqueue(q, &e);
}

static void synth_init(ImSynthQueue* q) {
	atomic_init(&q->read, 0);
	atomic_init(&q->write, 0);
	atomic_init(&q->overflow, false);
	q->api.program_change = synth_program;
	q->api.note_on = synth_on;
	q->api.note_off = synth_off;
	q->api.control_change = synth_control;
	q->api.pitch_bend = synth_bend;
	q->api.instrument = synth_instrument;
	q->api.sysex = synth_sysex;
}
#endif
