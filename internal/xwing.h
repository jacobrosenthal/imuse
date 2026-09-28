#ifndef IMUSE_INTERNAL_XWING_H
#define IMUSE_INTERNAL_XWING_H
#include "internal/players.h"
#include "internal/seq.h"
#include <imuse/xwing.h>
void ImXwing_SysEx(imuse_t* im, ImSeqData* seq, const uint8_t* data, size_t size);
void ImXwing_ApplyInstrument(imuse_t* im, ImMidiOutChannel* channel);
void ImXwing_SetRolandProgram(imuse_t* im, ImMidiPlayer* player, int channel, int program);
void ImXwing_SelectRolandPart(imuse_t* im, ImMidiPlayer* player, ImMidiOutChannel* channel);
int ImXwing_SetPackedHook(ImMidiPlayer* player, unsigned int hook);
void ImXwing_WriteMt32(imuse_t* im, unsigned int address, const uint8_t* data, size_t size);
#endif
