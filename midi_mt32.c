#include "internal/synth_queue.h"
#include <imuse/xwing.h>
#include <stdlib.h>
#ifdef HAVE_MT32EMU
#include <mt32emu/mt32emu.h>

typedef struct ImMt32 {
	ImSynthQueue q;
	mt32emu_context context;
	char* control;
	char* pcm;
	unsigned int delay;
} ImMt32;

static int mt_queued(ImMt32* b, mt32emu_return_code result) {
	if (result == MT32EMU_RC_OK)
		return 1;
	atomic_store_explicit(&b->q.overflow, true, memory_order_release);
	if (b->q.log)
		b->q.log(b->q.logUser, IMUSE_LOG_ERROR, "MT-32 MIDI queue overflow; output stopped");
	return 0;
}

static int mt_write(ImMt32* b, unsigned int address, const uint8_t* data, size_t size) {
	uint8_t wire[256] = { 0xf0, 0x41, 0x10, 0x16, 0x12 };
	wire[5] = (address >> 14) & 127;
	wire[6] = (address >> 7) & 127;
	wire[7] = address & 127;
	unsigned int sum = wire[5] + wire[6] + wire[7];
	for (size_t i = 0; i < size; ++i) {
		wire[8 + i] = data[i];
		sum += data[i];
	}
	wire[8 + size] = (uint8_t)(-sum & 127);
	wire[9 + size] = 0xf7;
	return mt_queued(b, mt32emu_play_sysex(b->context, wire, (unsigned int)size + 10));
}

static int mt_open(ImuseMidiBackend* api, int rate, ImuseLogFunc log, void* user) {
	ImMt32* b = (ImMt32*)api;
	b->q.log = log;
	b->q.logUser = user;
	mt32emu_report_handler_i handler = { 0 };
	b->context = mt32emu_create_context(handler, NULL);
	if (!b->context)
		return -1;
	if (mt32emu_add_rom_file(b->context, b->control) < 0 || mt32emu_add_rom_file(b->context, b->pcm) < 0) {
		if (log)
			log(user, IMUSE_LOG_ERROR, "MT-32: missing or incompatible control/PCM ROMs");
		mt32emu_free_context(b->context);
		b->context = NULL;
		return -1;
	}
	mt32emu_set_midi_event_queue_size(b->context, IM_SYNTH_QUEUE_SIZE * 2);
	mt32emu_set_stereo_output_samplerate(b->context, rate);
	if (mt32emu_open_synth(b->context) < 0) {
		mt32emu_free_context(b->context);
		b->context = NULL;
		return -1;
	}
	/* DOS93 0x1421a: eight melodic parts, rhythm on channel 9, reserve four each. */
	uint8_t system[23] = { 64, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 100 };
	/* Native slots use channels 0..7 instead of the DOS driver's 1..8. */
	for (int i = 0; i < 8; ++i)
		system[13 + i] = (uint8_t)i;
	if (!mt_write(b, 0x40000, system, sizeof system))
		goto startup_failed;
	static const uint8_t rhythm[44] = { 64, 100, 7,   0,   74, 100, 6,   0,   65, 100, 7,   0,   75, 100, 8,
										0,  69,  100, 6,   0,  68,  100, 11,  0,  81,  100, 5,   0,  67,  100,
										8,  0,   80,  100, 7,  0,   66,  100, 3,  0,   76,  100, 7,  0 };
	if (!mt_write(b, 0xc090, rhythm, sizeof rhythm))
		goto startup_failed;
	const uint8_t bend = 16;
	for (unsigned int i = 0; i < 32; ++i)
		if (!mt_write(b, 0x14004 + 8 * i, &bend, 1))
			goto startup_failed;
	b->q.rate = rate;
	return 0;

startup_failed:
	mt32emu_free_context(b->context);
	b->context = NULL;
	return -1;
}

static int mt_close(ImuseMidiBackend* api) {
	ImMt32* b = (ImMt32*)api;
	if (b->context)
		mt32emu_free_context(b->context);
	b->context = NULL;
	b->q.rate = 0;
	return 0;
}

static void mt_release(ImuseMidiBackend* api) {
	ImMt32* b = (ImMt32*)api;
	free(b->control);
	free(b->pcm);
	free(b);
}

static void mt_roland_program(ImuseMidiBackend* api, int slot, int program) {
	if ((unsigned int)slot >= 32 || (unsigned int)program >= 128)
		return;
	ImSynthEvent event = { .type = SYNTH_ROLAND_PROGRAM, .value = (uint16_t)slot, .a = (uint8_t)program };
	synth_enqueue(api, &event);
}

static void mt_roland_instrument(ImuseMidiBackend* api, int slot, const uint8_t* data, size_t size) {
	if ((unsigned int)slot >= 32 || !data || size != 246)
		return;
	ImSynthEvent event = { .type = SYNTH_ROLAND_INSTRUMENT, .value = (uint16_t)slot, .size = 246 };
	memcpy(event.data, data, size);
	synth_enqueue(api, &event);
}

static void mt_roland_select(ImuseMidiBackend* api, int channel, int slot) {
	if ((unsigned int)channel >= 8 || (unsigned int)slot >= 32)
		return;
	ImSynthEvent event = { .type = SYNTH_ROLAND_SELECT,
						   .channel = (uint8_t)channel,
						   .value = (uint16_t)slot };
	synth_enqueue(api, &event);
}

static void mt_roland_edit(ImuseMidiBackend* api, int slot, int channel, int offset, const uint8_t* data,
						   size_t size) {
	if ((unsigned int)slot >= 32 || channel < -1 || channel >= 8 || offset < 0 || (size != 1 && size != 2) ||
		(size_t)offset + size > 246 || !data)
		return;
	ImSynthEvent event = { .type = SYNTH_ROLAND_EDIT,
						   .channel = channel < 0 ? 0xff : (uint8_t)channel,
						   .value = (uint16_t)slot,
						   .a = (uint8_t)offset,
						   .size = (uint16_t)size };
	memcpy(event.data, data, size);
	synth_enqueue(api, &event);
}

static void mt_roland_bend(ImuseMidiBackend* api, int channel, int detune) {
	if ((unsigned int)channel >= 8)
		return;
	if (detune < -2048)
		detune = -2048;
	else if (detune > 2047)
		detune = 2047;
	ImSynthEvent event = { .type = SYNTH_BEND,
						   .channel = (uint8_t)channel,
						   .value = (uint16_t)((detune + 2048) * 4) };
	synth_enqueue(api, &event);
}

static void mt_apply(ImMt32* b, const ImSynthEvent* e) {
	unsigned int msg = 0;
	switch (e->type) {
		case SYNTH_ROLAND_PROGRAM: {
			uint8_t patch[2] = { e->a >> 6, e->a & 63 };
			mt_write(b, 0x14000u + 8u * e->value, patch, 2);
			b->delay = (unsigned int)b->q.rate / 100;
			return;
		}
		case SYNTH_ROLAND_INSTRUMENT: {
			uint8_t patch[2] = { 2, (uint8_t)e->value };
			if (!mt_write(b, 0x14000u + 8u * e->value, patch, 2))
				return;
			mt_write(b, 0x20000u + 256u * e->value, e->data, 246);
			b->delay = (unsigned int)b->q.rate * 60u / 1000u;
			return;
		}
		case SYNTH_ROLAND_SELECT:
			msg = 0xc0 | e->channel | ((unsigned int)e->value << 8);
			break;
		case SYNTH_ROLAND_EDIT:
			if (!mt_write(b, 0x20000u + 256u * e->value + e->a, e->data, e->size))
				return;
			if (e->channel < 8)
				mt_write(b, 0x10000u + 246u * e->channel + e->a, e->data, e->size);
			b->delay = (unsigned int)b->q.rate * (e->channel < 8 ? 20u : 10u) / 1000u;
			return;
		case SYNTH_SYSEX:
			mt_queued(b, mt32emu_play_sysex(b->context, e->data, e->size));
			b->delay = (unsigned int)b->q.rate / (e->size < 25 ? 100 : 20);
			return;
		case SYNTH_ON:
			if (e->channel > 15)
				return;
			msg = 0x90 | e->channel | (e->a << 8) | (e->b << 16);
			break;
		case SYNTH_OFF:
			if (e->channel > 15)
				return;
			msg = 0x80 | e->channel | (e->a << 8);
			break;
		case SYNTH_CONTROL:
			if (e->channel > 15)
				return;
			if (e->a == 17 || e->a == 18)
				return;
			/* The engine stores pan as 0..127; Roland's driver used -64..63. */
			msg = 0xb0 | e->channel | (e->a << 8) | ((unsigned int)(e->a == 10 ? 127 - e->b : e->b) << 16);
			break;
		case SYNTH_BEND:
			if (e->channel > 15)
				return;
			msg = 0xe0 | e->channel | ((e->value & 127) << 8) | ((e->value >> 7) << 16);
			break;
		default:
			return;
	}
	/* Munt's queue retries a NoteOn after partial abortion; immediate calls lose it. */
	mt_queued(b, mt32emu_play_msg(b->context, msg));
}

static void mt_render(ImuseMidiBackend* api, float* out, int frames) {
	ImMt32* b = (ImMt32*)api;
	if (!b->context || atomic_load_explicit(&b->q.overflow, memory_order_acquire)) {
		memset(out, 0, (size_t)frames * 2 * sizeof(float));
		return;
	}
	while (frames > 0) {
		ImSynthEvent event;
		while (!b->delay && !atomic_load_explicit(&b->q.overflow, memory_order_acquire) &&
			   synth_pop(&b->q, &event))
			mt_apply(b, &event);
		if (atomic_load_explicit(&b->q.overflow, memory_order_acquire)) {
			memset(out, 0, (size_t)frames * 2 * sizeof(float));
			return;
		}
		unsigned int count = b->delay && b->delay < (unsigned int)frames ? b->delay : (unsigned int)frames;
		mt32emu_render_float(b->context, out, count);
		if (b->delay)
			b->delay -= count;
		frames -= (int)count;
		out += count * 2;
	}
}

ImuseMidiBackend* imuse_midi_mt32_create(const char* control, const char* pcm) {
	if (!control || !*control || !pcm || !*pcm)
		return NULL;
	ImMt32* b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->control = malloc(strlen(control) + 1);
	b->pcm = malloc(strlen(pcm) + 1);
	if (!b->control || !b->pcm) {
		mt_release(&b->q.api);
		return NULL;
	}
	strcpy(b->control, control);
	strcpy(b->pcm, pcm);
	synth_init(&b->q);
	b->q.api.program_change = NULL;
	b->q.api.instrument = NULL;
	b->q.api.open = mt_open;
	b->q.api.close = mt_close;
	b->q.api.release = mt_release;
	b->q.api.render_float = mt_render;
	b->q.api.roland_program = mt_roland_program;
	b->q.api.roland_instrument = mt_roland_instrument;
	b->q.api.roland_select = mt_roland_select;
	b->q.api.roland_edit = mt_roland_edit;
	b->q.api.roland_bend = mt_roland_bend;
	b->q.api.melodicChannels = 8;
	return &b->q.api;
}
#else
ImuseMidiBackend* imuse_midi_mt32_create(const char* control, const char* pcm) {
	(void)control;
	(void)pcm;
	return NULL;
}
#endif
