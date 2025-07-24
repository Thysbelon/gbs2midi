/*
This file contains the definition of the gb_reg_write struct.
*/


// https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware
// https://gbdev.gg8.se/wiki/articles/Sound_Controller

#ifndef GBREGWRITE_H
#define GBREGWRITE_H

#ifdef __cplusplus
	extern "C" {
#endif

struct gb_reg_write {
	uint64_t time; // a timestamp relative to the start of the song. The unit of time used depends on the source of songData. gbsplay uses cycles as a unit of time (0x400000 cycles per second), while vgm files use samples as a unit of time (always 44100 samples per second for vgm files)
	uint8_t address; // memory address - 0xFF00. Example with NR10: 0xFF10 - 0xFF00 = 0x10
	uint8_t value;
};

#ifdef __cplusplus
	}
#endif

#endif