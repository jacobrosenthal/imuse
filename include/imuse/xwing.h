#ifndef IMUSE_XWING_H
#define IMUSE_XWING_H
#include <imuse/commands.h>
#include <imuse/midi_backend.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Validate the declared container and MIDI chunks; trailing LFD padding is allowed. */
int imuse_xwing_validate_sound(const void* data, size_t size);
/* Legacy hook classes: jump, transpose, part enable, volume, program, transpose.
 * Channel 16 sets all channels for a part hook. Values >=128 persist after use. */
int imuse_xwing_set_hook(imuse_t* im, intptr_t sound, int kind, int value, int channel);
int imuse_xwing_set_part_volume(imuse_t* im, intptr_t sound, int channel, int value);
ImuseMidiBackend* imuse_midi_xwing_adlib_create(void);
/* ROM paths are supplied by the host; the library distributes no Roland ROMs. */
ImuseMidiBackend* imuse_midi_mt32_create(const char* control_rom, const char* pcm_rom);
#ifdef __cplusplus
}
#endif
#endif
