/* DOS93 IMS command dialect. Addresses refer to DOS93_ADLIB/ROLAND.EXE.i64. */
#include "internal/xwing.h"
#include "internal/debug.h"
#include "internal/midi_backend.h"
#include "internal/slots.h"
#include "internal/state.h"
#include "internal/triggers.h"
#include "internal/utils.h"
#include <string.h>

static unsigned int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

static uint32_t be32(const uint8_t* p) { return ((uint32_t)be16(p) << 16) | be16(p + 2); }

int imuse_xwing_validate_sound(const void* bytes, size_t size) {
	const uint8_t* data = bytes;
	if (!data || size < 8 || (memcmp(data, "GMD ", 4) && memcmp(data, "ROL ", 4) && memcmp(data, "ADL ", 4)))
		return 0;
	size_t length = be32(data + 4), pos = 8;
	if (length > size - 8 || length > INT32_MAX - 8)
		return 0;
	size_t end = length + 8;
	int tracks = 0, header = 0;
	while (end - pos >= 8) {
		size_t count = be32(data + pos + 4);
		if (count > end - pos - 8)
			return 0;
		if (!memcmp(data + pos, "MDhd", 4))
			header = 1;
		if (!memcmp(data + pos, "MTrk", 4)) {
			if (!count)
				return 0;
			++tracks;
		}
		pos += 8 + count;
	}
	return pos == end && tracks && header;
}

static int set_hook(ImMidiPlayer* p, int kind, int value, int channel) {
	if (!p || kind < 0 || kind > 5 || value < 0 || value > 255 || channel < 0 || channel > 16)
		return -5;
	if (kind < 2)
		p->legacyHooks[kind][0] = (uint8_t)value;
	else if (channel == 16)
		memset(p->legacyHooks[kind], value, 16);
	else
		p->legacyHooks[kind][channel] = (uint8_t)value;
	return 0;
}

int imuse_xwing_set_hook(imuse_t* im, intptr_t sound, int kind, int value, int channel) {
	return im ? set_hook(ImPlayers_GetSoundPlayer(im, sound), kind, value, channel) : -4;
}

int ImXwing_SetPackedHook(ImMidiPlayer* p, unsigned int hook) {
	return hook < 256 ? set_hook(p, 0, hook, 0) : set_hook(p, 2, hook & 255, (hook >> 8) - 1);
}

int imuse_xwing_set_part_volume(imuse_t* im, intptr_t sound, int channel, int value) {
	if (!im || channel < 0 || channel > 15 || value < 0 || value > 127)
		return -5;
	ImMidiPlayer* p = ImPlayers_GetSoundPlayer(im, sound);
	if (!p)
		return -4;
	ImPlayers_SetParam(im, sound, IMUSE_PARAM_MIDI_PART_TRIM + channel, value);
	return 0;
}

/* 0x1204e..0x12243: wildcard 0 is unconditional; only 1..127 consume the hook. */
static int hook_matches(ImMidiPlayer* p, int kind, int channel, int value) {
	uint8_t* hook = &p->legacyHooks[kind][kind < 2 ? 0 : channel];
	if (value && value != *hook)
		return 0;
	if (value && value < 128)
		*hook = 0;
	return 1;
}

static size_t unpack(uint8_t* out, size_t capacity, const uint8_t* p, size_t size) {
	if ((size & 1) || size / 2 > capacity)
		return 0;
	for (size_t i = 0; i < size / 2; ++i) {
		if (p[2 * i] > 15 || p[2 * i + 1] > 15)
			return 0;
		out[i] = (uint8_t)((p[2 * i] << 4) | p[2 * i + 1]);
	}
	return size / 2;
}

void ImXwing_ApplyInstrument(imuse_t* im, ImMidiOutChannel* ch) {
	ImuseMidiBackend* b = im->midi.backend;
	if (ch->data && ch->instrumentSize && b && b->instrument)
		b->instrument(b, ch->data->outChannelId, ch->instrument, ch->instrumentSize);
}

static int roland_slot(imuse_t* im, ImMidiPlayer* player, ImMidiOutChannel* channel) {
	if (channel->rolandSlot >= 0 && channel->rolandSlot < 32)
		return channel->rolandSlot;
	uint32_t used = 0;
	for (ImMidiPlayer* active = im->players.list; active; active = active->next)
		for (int index = 0; index < 16; ++index)
			if (active->channels[index].rolandSlot >= 0 && active->channels[index].rolandSlot < 32)
				used |= 1u << active->channels[index].rolandSlot;
	for (int slot = 0; slot < 32; ++slot)
		if (!(used & (1u << slot))) {
			channel->rolandSlot = (int16_t)slot;
			return slot;
		}
	ImDebug_LogWarn(im, "MT-32 logical part pool exhausted for sound %lld", (long long)player->soundId);
	return -1;
}

void ImXwing_SetRolandProgram(imuse_t* im, ImMidiPlayer* player, int index, int program) {
	ImuseMidiBackend* backend = im->midi.backend;
	if (!backend || !backend->roland_program)
		return;
	int slot = roland_slot(im, player, &player->channels[index]);
	if (slot >= 0)
		backend->roland_program(backend, slot, program);
}

void ImXwing_SelectRolandPart(imuse_t* im, ImMidiPlayer* player, ImMidiOutChannel* channel) {
	ImuseMidiBackend* backend = im->midi.backend;
	if (!backend || !backend->roland_select || !channel->data)
		return;
	int slot = channel->rolandSlot;
	if (slot < 0 || slot >= 32) {
		slot = roland_slot(im, player, channel);
		if (slot < 0)
			return;
		if (channel->instrumentSize == 246 && backend->roland_instrument)
			backend->roland_instrument(backend, slot, channel->instrument, 246);
		else if (backend->roland_program && channel->partPgm < 128)
			backend->roland_program(backend, slot, channel->partPgm);
	}
	backend->roland_select(backend, channel->data->outChannelId, slot);
}

static void set_instrument(imuse_t* im, ImMidiPlayer* p, int channel, const uint8_t* data, size_t size) {
	if (channel > 15 || (size != 30 && size != 246))
		return;
	ImMidiOutChannel* ch = &p->channels[channel];
	ImPart* priorPart = ch->data;
	memcpy(ch->instrument, data, size);
	ch->instrumentSize = (uint16_t)size;
	ch->partStatus = 1;
	if (p->seqData->dialect == 3 && size == 246) {
		ImuseMidiBackend* backend = im->midi.backend;
		if (backend && backend->roland_instrument) {
			int slot = roland_slot(im, p, ch);
			if (slot >= 0)
				backend->roland_instrument(backend, slot, data, size);
		}
	}
	ImPlayers_MidiSetupParts(im);
	if (p->seqData->dialect == 3) {
		if (priorPart && ch->data == priorPart)
			ImXwing_SelectRolandPart(im, p, ch);
	} else
		ImXwing_ApplyInstrument(im, ch);
}

void ImXwing_WriteMt32(imuse_t* im, unsigned int address, const uint8_t* data, size_t size) {
	ImuseMidiBackend* b = im->midi.backend;
	if (!b || !b->sysex || size > 246)
		return;
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
	b->sysex(b, wire, size + 10);
}

static void global_instrument(imuse_t* im, int slot, const uint8_t* data, size_t size) {
	if (slot >= 32 || (size != 30 && size != 246))
		return;
	memcpy(im->xwing.instruments[slot], data, size);
	im->xwing.sizes[slot] = (uint16_t)size;
	/* Roland 0x1421a initializes global addresses to 0x22000 + slot*0x100. */
	if (size == 246)
		ImXwing_WriteMt32(im, 0x22000u + slot * 256u, data, size);
}

static void roland_sysex(imuse_t* im, ImMidiPlayer* p, const uint8_t* data, size_t size) {
	if (size < 10)
		return;
	unsigned int address = ((data[4] << 14) | (data[5] << 7) | data[6]) & 0xffffu;
	int channel = data[1];
	if (size == 255) {
		if (!address && channel < 16)
			set_instrument(im, p, channel, data + 7, 246);
		else if (address == 246)
			global_instrument(im, channel, data + 7, 246);
	} else if ((size == 10 || size == 11) && channel < 16 && address < 246) {
		ImMidiOutChannel* ch = &p->channels[channel];
		if (ch->instrumentSize != 246)
			return;
		ch->instrument[address] = data[7];
		if (size == 11 && address < 245)
			ch->instrument[address + 1] = data[8];
		ImuseMidiBackend* backend = im->midi.backend;
		if (backend && backend->roland_edit && ch->rolandSlot >= 0)
			backend->roland_edit(backend, ch->rolandSlot, ch->data ? ch->data->outChannelId : -1, address,
								 data + 7, size == 11 ? 2 : 1);
	}
}

static void control_hook(imuse_t* im, ImSeqData* seq, unsigned int op, int channel, const uint8_t* d,
						 size_t n) {
	ImMidiPlayer* p = seq->player;
	if (op == 0x30 && n >= 7 && !seq->scanning && hook_matches(p, 0, 0, d[0])) {
		unsigned int beat = be16(d + 3);
		if (!beat)
			beat = 1;
		ImSeq_Jump(im, seq, be16(d + 1) + 1, (beat - 1) / 4 + 1, (beat - 1) % 4 + 1, be16(d + 5), 1);
	} else if (op == 0x31 && n >= 3 && hook_matches(p, 1, 0, d[0])) {
		p->transpose = ImUtils_WrapSemitones(im, (d[1] ? p->transpose : 0) + (int8_t)d[2], -12, 12);
		for (int i = 0; i < 16; ++i)
			ImPlayers_HandleChannelDetuneChange(im, p, &p->channels[i]);
	} else if (op >= 0x32 && op <= 0x35 && n >= 2 && hook_matches(p, op - 0x30, channel, d[0])) {
		ImMidiOutChannel* ch = &p->channels[channel];
		if (op == 0x32) {
			ch->enabled = d[1] != 0;
			if (!ch->enabled)
				ImPlayers_MidiCommand(im, p, channel, 123, 0);
		} else if (op == 0x33)
			ImPlayers_MidiCommand(im, p, channel, 7, d[1]);
		else if (op == 0x34 && seq->dialect != 2)
			ImPlayers_ProgramChange(im, p, channel, d[1]);
		else if (op == 0x35 && n >= 3) {
			ch->legacyTranspose =
				(int8_t)ImUtils_WrapSemitones(im, (d[1] ? ch->legacyTranspose : 0) + (int8_t)d[2], -12, 12);
			ImPlayers_HandleChannelDetuneChange(im, p, ch);
		}
	}
}

void ImXwing_SysEx(imuse_t* im, ImSeqData* seq, const uint8_t* p, size_t size) {
	if (size < 3 || p[size - 1] != 0xf7)
		return;
	ImMidiPlayer* player = seq->player;
	if (p[0] == 0x41 && seq->dialect == 3) {
		roland_sysex(im, player, p, size);
		return;
	}
	if (p[0] != 0x7d || size < 4)
		return;
	unsigned int op = p[1], channel = p[2] & 15;
	uint8_t decoded[246];
	size_t n;
	if (op == 0x40 && !seq->scanning) {
		for (size_t i = 3; i + 1 < size; ++i) {
			intptr_t id = player->soundId;
			player->marker = p[i];
			ImTriggers_ProcessMarker(im, id, p[i]);
			if (im->seq.isEndOfTrack || player->soundId != id)
				return;
		}
	} else if (op == 0x10 && size >= 5 && p[3] == (seq->dialect == 2 ? 1 : seq->dialect == 3 ? 2 : 3)) {
		n = unpack(decoded, sizeof decoded, p + 4, size - 5);
		set_instrument(im, player, channel, decoded, n);
	} else if (op == 0x11 && size >= 6 && p[3] == (seq->dialect == 2 ? 1 : 2)) {
		n = unpack(decoded, sizeof decoded, p + 5, size - 6);
		global_instrument(im, p[4], decoded, n);
	} else if (op >= 0x30 && op <= 0x35) {
		n = unpack(decoded, sizeof decoded, p + 3, size - 4);
		control_hook(im, seq, op, channel, decoded, n);
	} else if (op == 0x60 && size == 8) {
		n = unpack(decoded, sizeof decoded, p + 3, 4);
		unsigned int slot = n == 2 ? be16(decoded) : 32;
		if (slot < 32)
			set_instrument(im, player, channel, im->xwing.instruments[slot], im->xwing.sizes[slot]);
	} else if (op == 0x50 && !seq->scanning) {
		n = unpack(decoded, sizeof decoded, p + 3, size - 4);
		if (n >= 10 && be16(decoded + 2) + 1 < be16(decoded + 6)) {
			seq->loopCount = be16(decoded);
			seq->loopChunk = seq->seqIndex;
			seq->loopBeat = be16(decoded + 2);
			seq->loopTick = be16(decoded + 4);
			seq->loopEndBeat = be16(decoded + 6);
			seq->loopEndTick = be16(decoded + 8);
		}
	} else if (op == 0x51)
		seq->loopCount = 0;
}
