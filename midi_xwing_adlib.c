/* Nine-voice OPL2 path recovered from DOS93 ADLIB.IMS 0x13f1e..0x14bca. */
#include "internal/synth_queue.h"
#include <imuse/xwing.h>
#include <stdlib.h>
#ifdef HAVE_FM4_OPL3
#include "internal/opl_buffer.h"
#include "internal/xwing_adlib_data.h"

typedef struct AdlibMod {
	int stage, duration, position, maximum, base, scale, speed;
	int ticks, remaining, step, remainder, direction, error, output;
	uint8_t flags, parameter, times[4], values[4];
} AdlibMod;

typedef struct AdlibVoice {
	int channel, note, velocity, duration, carrier, modulator, pitchOffset;
	int sustained;
	uint32_t age;
	uint8_t patch[30];
	AdlibMod mods[2];
} AdlibVoice;

typedef struct AdlibChannel {
	uint8_t patch[30];
	int valid, volume, bend, priority, modulation, sustain;
} AdlibChannel;

typedef struct ImAdlib {
	ImSynthQueue q;
	opl3_chip chip;
	AdlibChannel channels[32];
	AdlibVoice voices[9];
	uint8_t registers[256], random;
	unsigned int nextVoice, clock;
	uint32_t age;
} ImAdlib;

static const uint8_t operators[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void reg_write(ImAdlib* b, int reg, int value) {
	if (b->registers[reg] == (uint8_t)value)
		return;
	b->registers[reg] = (uint8_t)value;
	ImSynthEvent e = { .type = SYNTH_OPL_WRITE, .a = reg, .b = value };
	synth_enqueue(&b->q.api, &e);
}

static void voice_stop(ImAdlib* b, int i) {
	reg_write(b, 0xb0 + i, b->registers[0xb0 + i] & ~32);
	b->voices[i].channel = -1;
}

/* DOS93 0x10A58 builds this table with column zero cleared afterward. */
static int table_scale(int value, int scale) { return scale ? (value * (scale + 1)) >> 5 : 0; }

static int scale_value(int value, int scale) {
	if (!scale)
		return 0;
	if (scale == 31)
		return value;
	if (value >= -63 && value <= 63) {
		int result = table_scale(abs(value), abs(scale));
		return (value < 0) != (scale < 0) ? -result : result;
	}
	return ((value + 1) * scale) >> 5;
}

static int random_value(ImAdlib* b, int value) {
	b->random = (uint8_t)((b->random >> 1) ^ ((b->random & 1) ? 0xb8 : 0));
	return (b->random * value) >> 8;
}

static void pitch(ImAdlib* b, int i) {
	AdlibVoice* v = &b->voices[i];
	int value = clamp(v->note * 128 + b->channels[v->channel].bend + v->pitchOffset, 4 * 128, 127 * 128);
	int n = (value >> 7) - 4, frac = (value >> 4) & 7;
	int octave = adlib_octave[n] * 4, note = adlib_note[n];
	int old = b->registers[0xb0 + i] & ~32;
	if (b->registers[0xb0 + i] & 32) {
		if (octave > old && note < 6) {
			note += 12;
			octave -= 4;
		} else if (octave < old && note > 11) {
			note -= 12;
			octave += 4;
		}
	}
	reg_write(b, 0xa0 + i, adlib_fnum[clamp(note * 8 + frac, 0, 143)]);
	reg_write(b, 0xb0 + i, octave | 32);
}

static void parameter(ImAdlib* b, int i, int param, int value) {
	if (param == 28 || param == 29) {
		b->voices[i].pitchOffset = (value - (param == 28 ? 15 : 383)) * 16;
		pitch(b, i);
		return;
	}
	if (param < 0 || param > 27)
		return;
	int base = param < 13 ? operators[i] + 3 : param < 26 ? operators[i] : i;
	int index = (param < 13 ? param : param - 13) * 4;
	int reg = adlib_register_map[index] + base, shift = adlib_register_map[index + 1],
		mask = adlib_register_map[index + 2];
	if (adlib_register_map[index + 3])
		value = adlib_register_map[index + 3] - value;
	reg_write(b, reg, (b->registers[reg] & ~mask) | ((value << shift) & mask));
}

static int get_parameter(ImAdlib* b, int i, int param) {
	if (param == 28)
		return 15;
	if (param == 29)
		return 383;
	if (param < 0 || param > 27)
		return 0;
	int base = param < 13 ? operators[i] + 3 : param < 26 ? operators[i] : i;
	int index = (param < 13 ? param : param - 13) * 4;
	int value = (b->registers[adlib_register_map[index] + base] & adlib_register_map[index + 2]) >>
				adlib_register_map[index + 1];
	return adlib_register_map[index + 3] ? adlib_register_map[index + 3] - value : value;
}

static void volume(ImAdlib* b, int i) {
	AdlibVoice* v = &b->voices[i];
	int volume = b->channels[v->channel].volume >> 2;
	parameter(b, i, 0, adlib_volume[clamp(table_scale(v->carrier, volume), 0, 63)]);
	if (v->patch[10] & 1)
		parameter(b, i, 13, adlib_volume[clamp(table_scale(v->modulator, volume), 0, 63)]);
}

static void mod_stage(ImAdlib* b, AdlibMod* m) {
	int s = m->stage - 1, t = m->times[s];
	int index = clamp(table_scale(t & 127, m->speed), 0, 63);
	m->ticks = adlib_mod_steps[index * 2] | (adlib_mod_steps[index * 2 + 1] << 8);
	if (t & 128)
		m->ticks = random_value(b, m->ticks);
	if (!m->ticks)
		m->ticks = 1;
	m->remaining = m->ticks;
	int delta = 0;
	if (s != 2) {
		delta = scale_value(m->maximum, (m->values[s] & 127) - 31);
		if (m->values[s] & 128)
			delta = random_value(b, delta);
		delta = clamp(m->base + delta, 0, m->maximum) - m->base - m->position;
	}
	m->step = delta / m->ticks;
	m->direction = delta < 0 ? -1 : 1;
	m->remainder = abs(delta) % m->ticks;
	m->error = 0;
}

static void mod_init(ImAdlib* b, int i, int which, const uint8_t* p) {
	AdlibVoice* v = &b->voices[i];
	AdlibMod* m = &v->mods[which];
	memset(m, 0, sizeof(*m));
	if (!(p[0] & 128))
		return;
	int kind = p[0] & 15;
	if (kind >= 15)
		return;
	m->flags = p[0];
	m->parameter = adlib_mod_param[kind];
	m->maximum = adlib_mod_max[kind * 2] | (adlib_mod_max[kind * 2 + 1] << 8);
	m->base = m->parameter == 0    ? v->carrier
			  : m->parameter == 13 ? v->modulator
			  : m->parameter == 30 ? 31
			  : m->parameter == 31 ? 0
								   : get_parameter(b, i, m->parameter);
	m->scale = (p[0] & 64) ? b->channels[v->channel].modulation >> 2 : 31;
	m->speed = 31;
	m->duration = p[1] * 63;
	m->times[0] = p[2];
	m->times[1] = p[4];
	m->times[2] = p[6];
	m->times[3] = p[7];
	m->values[0] = p[3];
	m->values[1] = p[5];
	m->values[2] = 0;
	m->values[3] = p[8];
	m->stage = 1;
	mod_stage(b, m);
}

static void mod_tick(ImAdlib* b, int i, int which) {
	AdlibVoice* v = &b->voices[i];
	AdlibMod* m = &v->mods[which];
	if (!m->stage)
		return;
	if (m->duration && (m->duration -= 17) <= 0) {
		m->stage = 0;
		return;
	}
	m->position += m->step;
	m->error += m->remainder;
	if (m->error >= m->ticks) {
		m->error -= m->ticks;
		m->position += m->direction;
	}
	int output = scale_value(m->position, m->scale);
	if (output != m->output) {
		m->output = output;
		int value = clamp(m->base + output, 0, m->maximum);
		if (m->parameter == 0) {
			v->carrier = value;
			volume(b, i);
		} else if (m->parameter == 13) {
			v->modulator = value;
			volume(b, i);
		} else if (m->parameter == 30)
			v->mods[1 - which].scale = value;
		else if (m->parameter == 31)
			v->mods[1 - which].speed = value;
		else
			parameter(b, i, m->parameter, value);
	}
	if (--m->remaining == 0) {
		if (++m->stage > 4) {
			if (!(m->flags & 32)) {
				m->stage = 0;
				return;
			}
			m->stage = 1;
			if (m->flags & 16) {
				reg_write(b, 0xb0 + i, b->registers[0xb0 + i] & ~32);
				reg_write(b, 0xb0 + i, b->registers[0xb0 + i] | 32);
			}
		}
		mod_stage(b, m);
	}
}

static void note_on(ImAdlib* b, int channel, int note, int velocity) {
	AdlibChannel* c = &b->channels[channel];
	if (!c->valid)
		return;
	int selected = -1, priority = c->priority;
	/* 0x144f6: round-robin free voice; steal the oldest note of an eligible part. */
	for (int n = 0; n < 9; ++n) {
		int i = (int)(b->nextVoice = (b->nextVoice + 1) % 9);
		AdlibVoice* v = &b->voices[i];
		if (v->channel < 0) {
			selected = i;
			break;
		}
		int oldest = 1;
		for (int j = 0; j < 9; ++j)
			if (b->voices[j].channel == v->channel && b->voices[j].age < v->age)
				oldest = 0;
		if (oldest && b->channels[v->channel].priority <= priority) {
			selected = i;
			priority = b->channels[v->channel].priority;
		}
	}
	if (selected < 0)
		return;
	voice_stop(b, selected);
	AdlibVoice* v = &b->voices[selected];
	memset(v, 0, sizeof(*v));
	v->channel = channel;
	v->note = note;
	v->velocity = velocity;
	v->age = ++b->age;
	memcpy(v->patch, c->patch, 30);
	uint8_t* p = v->patch;
	v->duration = p[29] * 63;
	v->modulator = clamp((p[1] & 63) + table_scale(velocity >> 1, p[4] >> 2), 0, 63);
	v->carrier = clamp((p[6] & 63) + table_scale(velocity >> 1, p[9] >> 2), 0, 63);
	int levels[2] = { v->modulator, adlib_volume[table_scale(v->carrier, c->volume >> 2)] };
	if (p[10] & 1)
		levels[0] = adlib_volume[table_scale(v->modulator, c->volume >> 2)];
	for (int op = 0; op < 2; ++op) {
		int r = operators[selected] + op * 3, off = op * 5;
		reg_write(b, 0x20 + r, p[off]);
		reg_write(b, 0x40 + r, (p[off + 1] | 63) - levels[op]);
		reg_write(b, 0x60 + r, ~p[off + 2]);
		reg_write(b, 0x80 + r, ~p[off + 3]);
		reg_write(b, 0xe0 + r, p[off + 4]);
	}
	reg_write(b, 0xc0 + selected, p[10]);
	pitch(b, selected);
	mod_init(b, selected, 0, p + 11);
	mod_init(b, selected, 1, p + 20);
}

static void adlib_apply(ImAdlib* b, const ImSynthEvent* e) {
	if (e->channel >= 32)
		return;
	AdlibChannel* c = &b->channels[e->channel];
	if (e->type == SYNTH_INSTRUMENT && e->size == 30) {
		memcpy(c->patch, e->data, 30);
		c->valid = 1;
		return;
	}
	if (e->type == SYNTH_ON && e->b) {
		note_on(b, e->channel, e->a, e->b);
		return;
	}
	if (e->type == SYNTH_CONTROL) {
		if (e->a == 7)
			c->volume = e->b;
		else if (e->a == 18)
			c->priority = e->b;
		else if (e->a == 1)
			c->modulation = e->b;
		else if (e->a == 64)
			c->sustain = e->b;
		else if (e->a == 123 || e->a == 120)
			c->sustain = 0;
	}
	if (e->type == SYNTH_BEND)
		c->bend = (int16_t)e->value;
	for (int i = 0; i < 9; ++i)
		if (b->voices[i].channel == e->channel) {
			if ((e->type == SYNTH_OFF || (e->type == SYNTH_ON && !e->b)) && b->voices[i].note == e->a) {
				if (c->sustain)
					b->voices[i].sustained = 1;
				else
					voice_stop(b, i);
			} else if (e->type == SYNTH_CONTROL && (e->a == 123 || e->a == 120))
				voice_stop(b, i);
			else if (e->type == SYNTH_CONTROL && e->a == 64 && !c->sustain && b->voices[i].sustained)
				voice_stop(b, i);
			else if (e->type == SYNTH_CONTROL && e->a == 7)
				volume(b, i);
			else if (e->type == SYNTH_CONTROL && e->a == 1) {
				for (int m = 0; m < 2; ++m)
					if (b->voices[i].mods[m].flags & 64)
						b->voices[i].mods[m].scale = e->b >> 2;
			} else if (e->type == SYNTH_BEND)
				pitch(b, i);
		}
}

/* Driver state stays with the sequencer, allowing an exact live-voice census.
 * Only register/value copies enter the SPSC queue consumed by the OPL thread. */
static void adlib_on(ImuseMidiBackend* api, int channel, int note, int velocity) {
	ImSynthEvent e = { .type = SYNTH_ON, .channel = channel, .a = note, .b = velocity };
	adlib_apply((ImAdlib*)api, &e);
}

static void adlib_off(ImuseMidiBackend* api, int channel, int note) {
	ImSynthEvent e = { .type = SYNTH_OFF, .channel = channel, .a = note };
	adlib_apply((ImAdlib*)api, &e);
}

static void adlib_control(ImuseMidiBackend* api, int channel, unsigned int control, int value) {
	ImSynthEvent e = { .type = SYNTH_CONTROL, .channel = channel, .a = control, .b = value };
	adlib_apply((ImAdlib*)api, &e);
}

static void adlib_bend(ImuseMidiBackend* api, int channel, int detune) {
	ImSynthEvent e = { .type = SYNTH_BEND, .channel = channel, .value = (uint16_t)detune };
	adlib_apply((ImAdlib*)api, &e);
}

static void adlib_wire_bend(ImuseMidiBackend* api, int channel, int value) {
	int bend = value - 8192;
	adlib_bend(api, channel, bend >= 0 ? bend / 4 : -((-bend + 3) / 4));
}

static void adlib_instrument(ImuseMidiBackend* api, int channel, const uint8_t* data, size_t size) {
	if (size != 30)
		return;
	ImSynthEvent e = { .type = SYNTH_INSTRUMENT, .channel = channel, .size = size };
	memcpy(e.data, data, size);
	adlib_apply((ImAdlib*)api, &e);
}

static void adlib_notes(ImuseMidiBackend* api, unsigned int channels, unsigned int notes[128]) {
	ImAdlib* b = (ImAdlib*)api;
	memset(notes, 0, 128 * sizeof(*notes));
	for (int i = 0; i < 9; ++i) {
		const AdlibVoice* v = &b->voices[i];
		if (v->channel >= 0 && (channels & (1u << v->channel)))
			notes[v->note] |= 1u << v->channel;
	}
}

static void adlib_service(ImuseMidiBackend* api) {
	ImAdlib* b = (ImAdlib*)api;
	b->clock += 3433;
	if (b->clock < 16667)
		return;
	b->clock -= 16667;
	for (int i = 0; i < 9; ++i) {
		AdlibVoice* v = &b->voices[i];
		if (v->channel < 0)
			continue;
		if (v->duration && (v->duration -= 17) <= 0) {
			voice_stop(b, i);
			return; /* 0x14092 stops this service after an expired voice. */
		}
		mod_tick(b, i, 0);
		mod_tick(b, i, 1);
	}
}

static int adlib_open(ImuseMidiBackend* api, int rate, ImuseLogFunc log, void* user) {
	ImAdlib* b = (ImAdlib*)api;
	b->q.rate = rate;
	b->q.log = log;
	b->q.logUser = user;
	b->random = 1;
	OPL3_Reset(&b->chip, rate);
	reg_write(b, 1, 32);
	reg_write(b, 8, 64);
	reg_write(b, 0xbd, 0);
	for (int i = 0; i < 32; ++i) {
		b->channels[i].volume = 127;
	}
	for (int i = 0; i < 9; ++i)
		b->voices[i].channel = -1;
	return 0;
}

static int adlib_close(ImuseMidiBackend* api) {
	((ImAdlib*)api)->q.rate = 0;
	return 0;
}

static void adlib_release(ImuseMidiBackend* api) { free(api); }

static void adlib_render(ImuseMidiBackend* api, float* out, int frames) {
	ImAdlib* b = (ImAdlib*)api;
	if (atomic_load_explicit(&b->q.overflow, memory_order_acquire)) {
		memset(out, 0, (size_t)frames * 2 * sizeof(float));
		return;
	}
	for (int f = 0; f < frames; ++f) {
		ImSynthEvent e;
		while (im_opl_write_space(&b->chip) && synth_pop(&b->q, &e))
			OPL3_WriteRegBuffered(&b->chip, e.a, e.b);
		int16_t sample[2];
		OPL3_GenerateResampled(&b->chip, sample);
		out[2 * f] = sample[0] / 32768.0f;
		out[2 * f + 1] = sample[1] / 32768.0f;
	}
}

ImuseMidiBackend* imuse_midi_xwing_adlib_create(void) {
	ImAdlib* b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	synth_init(&b->q);
	b->q.api.open = adlib_open;
	b->q.api.close = adlib_close;
	b->q.api.release = adlib_release;
	b->q.api.render_float = adlib_render;
	b->q.api.program_change = NULL;
	b->q.api.sysex = NULL;
	b->q.api.note_on = adlib_on;
	b->q.api.note_off = adlib_off;
	b->q.api.control_change = adlib_control;
	b->q.api.pitch_bend = adlib_wire_bend;
	b->q.api.instrument = adlib_instrument;
	b->q.api.adlib_service = adlib_service;
	b->q.api.adlib_bend = adlib_bend;
	b->q.api.adlib_notes = adlib_notes;
	b->q.api.melodicChannels = 32;
	return &b->q.api;
}
#else
ImuseMidiBackend* imuse_midi_xwing_adlib_create(void) { return NULL; }
#endif
