/*
This file contains the code that converts songData to a midi file.

info on sysex data structure:
All wave data is stored in a single sysex message at the beginning of the song.
The wave data consists of values from 0x00 to 0x0F.
example of a sysex message that contains two waves:
F0
0F 0F 0F 0F 0F 0D 0B 08 05 03 01 00 00 00 00 00 00 00 00 00 00 01 03 05 08 0B 0D 0F 0F 0F 0F 0F 
00 00 00 00 00 00 00 00 00 00 00 00 0F 0F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 0F 00 00 
F7
(line breaks added.)

Throughout the song, the index of the current wave to use will be selected with CC21


notes on notes:
when to start notes:
A note is started by triggering the channel. (legato should be turned off when a channel is triggered)
changing the pitch past the point that it can be captured with a pitch bend event should also start a new note, but with legato on.

when to end notes:
- when a note is currently playing and a new note has started.
- conditions that would turn off a gb channel:
	when sound length is enabled and the end of sound length has been reached.
	"For CH1 only: when the period sweep overflows"
	when the DAC is turned off, which is accomplished by setting NRx2 bits 7-3 to 0 (for env channels) or by setting NR30 bit 7 to 0 (NOTE: when the DAC is turned off, triggering the channel doesn't turn DAC or the channel back on. DAC can be re-enabled using the same registers and bits used to disable it.)

Ideally, we would end notes when volume is set to 0, but that could conflict with a song where the volume goes up and down.

Above all, we should avoid a situation where one note accidentally lasts the length of the whole song, while other notes are playing.
*/

#include <cstdint>
#include <string>
#include <cstdio>
#include <vector>
#include <cmath>
#include <set>
#include <array>
#include <tuple>
#include <cstring>
#include <variant>
#include <algorithm> // std::find
#include <map>
#include <chrono> // for measuring performance

#include "gb_chip_state.hpp"
#include "libsmfc.h"
#include "libsmfcx.h"

#include "to_midi.hpp"

std::vector<uint8_t> NOISE_PITCH_LIST; // this variable is global so that all functions can access it without me needing to pass it in.
uint64_t midiTicksPerSoundLenTick = 1;
std::vector<gb_reg_write>* songDataPointer;
int* regWriteIpointer;
unsigned int* gbTimeUnitsPerSecondPointer;
const uint64_t* midiTicksPerSecondPointer;

static uint64_t gbTime2midiTime(uint64_t gbTime /*timestamp relative to start of song*/, unsigned int gbTimeUnitsPerSecond, const uint64_t midiTicksPerSecond){
	double gbTimeInSeconds = gbTime / (double)gbTimeUnitsPerSecond;
	uint64_t midiTime = llround(gbTimeInSeconds * midiTicksPerSecond);
	return midiTime;
}
static uint16_t combinePitch(uint8_t inPitchMSB, uint8_t inPitchLSB){
	return (uint16_t)(inPitchLSB) | ((uint16_t)(inPitchMSB) << 8);
}
static int closest(std::vector<uint16_t> const& vec, int value) { // https://stackoverflow.com/questions/8647635/elegant-way-to-find-closest-value-in-a-vector-from-above
	auto const it = std::lower_bound(vec.begin(), vec.end(), value);
	if (it == vec.end()) { return -1; }
	return *it;
}
static std::pair<int, int> gbPitch2noteAndPitch(uint16_t gbPitch){ // https://www.devrs.com/gb/files/sndtab.html
	int note;
	int pitchAdjust;
	std::vector<uint16_t> gbPitchArray = {44,156,262,363,457,547,631,710,786,854,923,986,1046,1102,1155,1205,1253,1297,1339,1379,1417,1452,1486,1517,1546,1575,1602,1627,1650,1673,1694,1714,1732,1750,1767,1783,1798,1812,1825,1837,1849,1860,1871,1881,1890,1899,1907,1915,1923,1930,1936,1943,1949,1954,1959,1964,1969,1974,1978,1982,1985,1988,1992,1995,1998,2001,2004,2006,2009,2011,2013,2015}; // length: 72
	uint8_t noteC2=36; // midi note number
	uint16_t closestGbpitch = closest(gbPitchArray, gbPitch);
	uint8_t gbPitchArrayIndex=std::distance( gbPitchArray.begin(), std::find(gbPitchArray.begin(), gbPitchArray.end(), closestGbpitch) );
	note = noteC2 + gbPitchArrayIndex;
	int pitchDifference = (int)gbPitch - closestGbpitch;
	
	
	if (pitchDifference > 0) {
		if (gbPitchArrayIndex+1 >= gbPitchArray.size()) {
			pitchAdjust = 0;
		} else {
			uint16_t totalSemitoneDiff = gbPitchArray[gbPitchArrayIndex+1] - gbPitchArray[gbPitchArrayIndex];
			pitchAdjust = 0 + (float)0x1000 * ((float)pitchDifference / totalSemitoneDiff);
		}
	} else if (pitchDifference < 0) {
		//printf("negative pitchDifference: %d\n", pitchDifference);
		if (gbPitchArrayIndex <= 0) {
			pitchAdjust = 0;
		} else {
			uint16_t totalSemitoneDiff = gbPitchArray[gbPitchArrayIndex] - gbPitchArray[gbPitchArrayIndex-1];
			int pitchAdjustAlter = (float)0x1000 * ((float)std::abs(pitchDifference) / totalSemitoneDiff);
			if (pitchAdjustAlter > 0x1000 / 2) { // TODO: make this checks possible to disable on the command line.
				note--;
				pitchAdjust = 0x1000 - pitchAdjustAlter;
			} else {
				pitchAdjust = 0 - pitchAdjustAlter;
			}
			//printf("totalSemitoneDiff: %u, pitchAdjust: %d\n", totalSemitoneDiff, pitchAdjust);
		}
	} else {
		pitchAdjust = 0;
	}
	
	return std::make_pair(note, pitchAdjust);
}
static void insertNoteIntoMidi(const uint8_t newNote, const uint8_t channel, std::array<uint8_t,4>& curPlayingMidiNote, const uint64_t& regWriteMidiTime, Smf* midiFile, const uint16_t prevRegPitch){ // ends the currently playing note and inserts a new note.
	int tempCheckAddress = channel*0x5 + 0x10;
	bool doNotInsertNote = false;
	for (int i = (*regWriteIpointer) + 1; i < songDataPointer->size(); i++){ // if any of the upcoming regWrites both happen at the same time as this one AND would also cause a note to be inserted, don't do anything yet. This is necessary in order to prevent accidentally inserting long, overlapping notes into the midi. BUG: because this only keeps certain notes, this has produced a new bug where sometimes the "wrong" notes will be preserved and the song sounds off. HOWEVER, this only seems to be an issue when the PPQN is low.
		uint64_t nextRegWriteMidiTime = gbTime2midiTime((*songDataPointer)[i].time, *gbTimeUnitsPerSecondPointer, *midiTicksPerSecondPointer);
		if (nextRegWriteMidiTime != regWriteMidiTime) 
			break;
		int nextRegAddress = (*songDataPointer)[i].address;
		if (nextRegAddress == (tempCheckAddress+3) || nextRegAddress == (tempCheckAddress+4)) {
			/*
			if (channel == 2) { 
				printf("There is another pitch reg write at the same time. nextRegAddress: %02X\n", nextRegAddress);
			}
			*/
			// TODO: remove redundant code.
			uint16_t nextRegPitch = 0;
			uint8_t nextTrigger=0;
			if (channel != 3){ // simply checking if the next regWrite would change pitch is not enough. I need to check if it would actually change the midi note.
				if (nextRegAddress == (tempCheckAddress+3)) {
					nextRegPitch = combinePitch((prevRegPitch & 0b11100000000) >> 8, (*songDataPointer)[i].value);
				} else if (nextRegAddress == (tempCheckAddress+4)) {
					nextRegPitch = combinePitch((*songDataPointer)[i].value & 0b111, prevRegPitch & 0xFF);
					nextTrigger = (*songDataPointer)[i].value & 0b10000000;
				}
				uint8_t nextMidiNote = gbPitch2noteAndPitch(nextRegPitch).first;
				if (nextMidiNote != curPlayingMidiNote[channel] || nextTrigger){
					doNotInsertNote = true;
				}
			} else {
				doNotInsertNote = true; // TODO: TEST
			}
			break;
		}
	}
	/*
	if (channel == 2) { 
		printf("doNotInsertNote: %d\n", doNotInsertNote);
	}
	*/
	if (doNotInsertNote == false) {
		if (curPlayingMidiNote[channel] != 0xFF){ // a note is playing
			// end the currently playing note
			smfInsertNoteOff(midiFile, regWriteMidiTime, channel, channel, curPlayingMidiNote[channel], 0x7F);
		}
		// insert new note
		smfInsertNoteOn(midiFile, regWriteMidiTime, channel, channel, newNote, 0x7F);
		curPlayingMidiNote[channel] = newNote; // a new note has started. Put it in the array to keep track of it.
		//if (channel == 2) printf("regWriteMidiTime: %lu. nextRegWriteMidiTime: %lu. nextRegWriteMidiTime == regWriteMidiTime: %d. nextRegAddress: 0x%02X\n", regWriteMidiTime, nextRegWriteMidiTime, nextRegWriteMidiTime == regWriteMidiTime, nextRegAddress);
	}
}
static uint8_t noisePitch2note(uint8_t noisePitch){
	uint8_t note = std::distance(NOISE_PITCH_LIST.begin(), std::find(NOISE_PITCH_LIST.begin(), NOISE_PITCH_LIST.end(), noisePitch));
	return note;
}
static uint8_t extractBitValueFromByte(const uint8_t inByte, uint8_t startBit, uint8_t endBit) {
	if (startBit > 7) startBit = 7;
	if (endBit > 7) endBit = 7;
	if (endBit > startBit) {
		fprintf(stderr, "extractBitValueFromByte called with a higher endBit than startBit.\n");
		return 1;
	}
	uint8_t targetBits=0;
	//targetBits |= (1 << startBit); // targetBits |= (1 << 7)  -->  targetBits == 0b10000000
	for (int8_t i=startBit; i >= endBit; i--) {
		targetBits |= (1 << i);
	}
	uint8_t outVal = inByte & targetBits;
	outVal = outVal >> endBit;
	return outVal;
}
static uint8_t convertValToMidiCCrange(uint8_t inVal, uint8_t inValMax){
	const uint8_t MIDI_CC_MAX = 0x7F;
	return (uint8_t)round((float)MIDI_CC_MAX * ((float)inVal / inValMax));
}
static void handleCommonRegWrite(const uint8_t inRegWriteVal, std::vector<std::pair<uint8_t, bool>*> propertyVector, const std::vector<std::pair<uint8_t, uint8_t>> bitRangeVector, const std::vector<uint8_t> midiCCvector, const uint8_t channel, const uint64_t& regWriteMidiTime, Smf* midiFile){
	for (int i=0; i<midiCCvector.size(); i++){ // all the vectors should be the same size
		uint8_t regBitVal = extractBitValueFromByte(inRegWriteVal, bitRangeVector[i].first, bitRangeVector[i].second);
		uint8_t regBitValMax = extractBitValueFromByte(0xFF, bitRangeVector[i].first, bitRangeVector[i].second);
		if ((*(propertyVector[i])).first != regBitVal || (*(propertyVector[i])).second == false)
			smfInsertControl(midiFile, regWriteMidiTime, channel, channel, midiCCvector[i], convertValToMidiCCrange(regBitVal, regBitValMax));
		*(propertyVector[i]) = std::make_pair(regBitVal, true); // change the value that is pointed to. Write the new value to the APU state
	}
}
static void handleSqDutyAndSoundLen(const uint8_t inRegWriteVal, gb_chip_state::square_channels* chanState, const uint8_t channel, const uint64_t& regWriteMidiTime, Smf* midiFile){
	std::vector<std::pair<uint8_t, bool>*> propertyVector;
	std::vector<std::pair<uint8_t, uint8_t>> bitRangeVector;
	std::vector<uint8_t> midiCCvector;
	propertyVector = {&(chanState->duty_cycle), &(chanState->sound_length) /*this may need a special dynamic_cast*/};
	bitRangeVector = {std::make_pair(7,6), std::make_pair(5,0)};
	midiCCvector = {19, 15};
	handleCommonRegWrite(inRegWriteVal, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile);
}
static void handleEnv(const uint8_t inRegWriteVal, gb_chip_state::channels_with_env* chanState, const uint8_t channel, const uint64_t& regWriteMidiTime, Smf* midiFile){
	std::vector<std::pair<uint8_t, bool>*> propertyVector;
	std::vector<std::pair<uint8_t, uint8_t>> bitRangeVector;
	std::vector<uint8_t> midiCCvector;
	propertyVector = {&(chanState->env_start_vol), &(chanState->env_down_or_up), &(chanState->env_length)};
	bitRangeVector = {std::make_pair(7,4), std::make_pair(3,3), std::make_pair(2,0)};
	midiCCvector = {SMF_CONTROL_VOLUME, 12, 13};
	handleCommonRegWrite(inRegWriteVal, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile);
}
static void handlePitchBend(uint16_t curRegPitch, uint16_t prevRegPitch, bool isPitchValid, Smf* midiFile, const uint64_t& regWriteMidiTime, const uint8_t channel, std::array<uint8_t,4>& curPlayingMidiNote, bool& chanLegato /*legatoState*/){
	if (isPitchValid) {
		if (curRegPitch != prevRegPitch) {
			// calculate note and pitchAdjust
			std::pair<int, int> noteAndPitchAdjust = gbPitch2noteAndPitch(curRegPitch);
			// insert pitch bend
			smfInsertPitchBend(midiFile, regWriteMidiTime, channel, channel, noteAndPitchAdjust.second);
			int prevMidiNote = curPlayingMidiNote[channel]; // TODO: just pass curPlayingMidiNote[channel] as an argument (as a modifiable reference), instead of passing the whole array?
			if (noteAndPitchAdjust.first != prevMidiNote) {
				insertNoteIntoMidi(noteAndPitchAdjust.first, channel, curPlayingMidiNote, regWriteMidiTime, midiFile, prevRegPitch);
				//printf("noteAndPitchAdjust.first == curPlayingMidiNote[channel]: %d\n", noteAndPitchAdjust.first == curPlayingMidiNote[channel]); // after insertNoteIntoMidi() runs, these should be equal
				if (chanLegato==false) {
					smfInsertControl(midiFile, regWriteMidiTime, channel, channel, 68, 0x7F);
					chanLegato=true;
				}
			}
		}
	}
}
static void handlePitchLSB(const uint8_t inRegWriteVal, gb_chip_state::melodic_channels* chanState, const uint8_t channel, const uint64_t& regWriteMidiTime, Smf* midiFile, std::array<uint8_t,4>& curPlayingMidiNote, bool& chanLegato){
	bool isPitchValid = chanState->pitchMSB.second; // we know that pitchLSB is valid because it's being written to right now.
	
	uint16_t curRegPitch = combinePitch(chanState->pitchMSB.first, inRegWriteVal);
	uint16_t prevRegPitch = chanState->getPitch();
	handlePitchBend(curRegPitch, prevRegPitch, isPitchValid, midiFile, regWriteMidiTime, channel, curPlayingMidiNote, chanLegato);
	chanState->pitchLSB = std::make_pair(inRegWriteVal, true);
}
static void handlePitchMSBtriggerSoundLenEnable(const uint8_t inRegWriteVal, gb_chip_state::base_chan_class* chanState, const uint8_t channel, const uint64_t& regWriteMidiTime, Smf* midiFile, std::array<uint8_t,4>& curPlayingMidiNote, bool& chanLegato, std::array<uint64_t,4>& scheduledSoundLenEndTime){
	std::vector<std::pair<uint8_t, bool>*> propertyVector = {&(chanState->sound_length_enable)};
	std::vector<std::pair<uint8_t, uint8_t>> bitRangeVector = {std::make_pair(6,6)};
	std::vector<uint8_t> midiCCvector = {14};
	handleCommonRegWrite(inRegWriteVal, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile);
	
	uint8_t trigger = extractBitValueFromByte(inRegWriteVal, 7, 7);
	uint8_t pitchMSB=0;
	uint16_t curRegPitch=0;
	
	bool isPitchValid = false;
	if (channel!=3) {
		isPitchValid = dynamic_cast<gb_chip_state::melodic_channels*>(chanState)->pitchLSB.second; // we know that pitchMSB is valid because it's being written to right now.
		pitchMSB = extractBitValueFromByte(inRegWriteVal, 2, 0);
		curRegPitch = combinePitch(pitchMSB, dynamic_cast<gb_chip_state::melodic_channels*>(chanState)->pitchLSB.first);
	} else {
		curRegPitch = dynamic_cast<gb_chip_state::noise*>(chanState)->noise_pitch.first;
	}
	
	if (trigger==1){
		// set scheduledSoundLenEndTime
		if (chanState->sound_length_enable.first && chanState->sound_length_enable.second && chanState->sound_length.second) {
			scheduledSoundLenEndTime[channel] = regWriteMidiTime + ((channel == 2 ? 256 : 64) - chanState->sound_length.first) * midiTicksPerSoundLenTick; // in the main loop, check scheduledSoundLenEndTime for all channels and see if any of them are in the past compared to regWriteMidiTime. If yes, insert a noteOff at that scheduledSoundLenEndTime. It should be okay to insert midi events at any time in any order.
			// if a note retriggers before its scheduledSoundLenEndTime arrives, that scheduledSoundLenEndTime will be overwritten, thus a channel will only do a note off if it actually reaches its scheduledSoundLenEndTime without retriggering.
		}
		
		if (chanLegato==true) {
			smfInsertControl(midiFile, regWriteMidiTime, channel, channel, 68, 0);
			chanLegato=false;
		}
		// end previous note
		// insert note
		int note=0;
		uint16_t prevRegPitch = 0;
		if (channel!=3) {
			std::pair<int, int> noteAndPitchAdjust;
			noteAndPitchAdjust = gbPitch2noteAndPitch(curRegPitch);
			smfInsertPitchBend(midiFile, regWriteMidiTime, channel, channel, noteAndPitchAdjust.second);
			note = noteAndPitchAdjust.first;
			prevRegPitch = dynamic_cast<gb_chip_state::melodic_channels*>(chanState)->getPitch();
		} else {
			note = noisePitch2note((uint8_t)curRegPitch);
			prevRegPitch = curRegPitch;
		}
		insertNoteIntoMidi(note, channel, curPlayingMidiNote, regWriteMidiTime, midiFile, prevRegPitch);
	} else {
		if (channel!=3) {
			uint16_t prevRegPitch = dynamic_cast<gb_chip_state::melodic_channels*>(chanState)->getPitch();
			handlePitchBend(curRegPitch, prevRegPitch, isPitchValid, midiFile, regWriteMidiTime, channel, curPlayingMidiNote, chanLegato);
		}
	}
	if (channel!=3) dynamic_cast<gb_chip_state::melodic_channels*>(chanState)->pitchMSB = std::make_pair(pitchMSB, true);
}
static void handlePanning(gb_chip_state* curAPUstate, const uint8_t inRegWriteVal, Smf* midiFile, const uint64_t& regWriteMidiTime){
	std::vector<gb_chip_state::base_chan_class*> channelPointerVector = {&(curAPUstate->gb_square1_state), &(curAPUstate->gb_square2_state), &(curAPUstate->gb_wave_state), &(curAPUstate->gb_noise_state)};
	for (int i=0; i<4; i++){
		uint8_t panningRegVal = ((inRegWriteVal >> (3+i)) & 0b10) | ((inRegWriteVal >> i) & 0b01);
		if (panningRegVal != channelPointerVector[i]->panning.first || channelPointerVector[i]->panning.second == false) {
			if (panningRegVal == 0) {
				smfInsertControl(midiFile, regWriteMidiTime, i, i, 9, 0x7F); // pan mute on
			} else if (panningRegVal != 0) {
				if (channelPointerVector[i]->panning.first == 0 || channelPointerVector[i]->panning.second == false)
					smfInsertControl(midiFile, regWriteMidiTime, i, i, 9, 0); // pan mute off
				uint8_t midiPan=0;
				switch (panningRegVal){
					case 0b01:
						midiPan=0x7F;
						break;
					case 0b10:
						midiPan=0;
						break;
					case 0b11:
						midiPan=64;
						break;
				}
				smfInsertControl(midiFile, regWriteMidiTime, i, i, SMF_CONTROL_PANPOT, midiPan);
			}
		}
		channelPointerVector[i]->panning = std::make_pair(panningRegVal, true);
	}
}
bool songData2midi(std::vector<gb_reg_write>& songData, unsigned int gbTimeUnitsPerSecond, std::string outfilename){
	auto start = std::chrono::high_resolution_clock::now();
	
	std::vector<uint8_t> tempNoisePitchList;
	for (int16_t noisePitch=0xF7; noisePitch >= 0; noisePitch--){ // the list should be written backwards because lower values tend to be higher pitched. 0xF7 is 0b11110111.
		if ((noisePitch & 8) == 0) {
			tempNoisePitchList.push_back(noisePitch);
		}
	}
	NOISE_PITCH_LIST = tempNoisePitchList;
	
	songDataPointer = &songData;
	
	const int SECONDS_IN_A_MINUTE=60;
	const int MIDI_BPM=120;
	const int MIDI_PPQN=0x7fff;
	//const int MIDI_PPQN=99;
	Smf* midiFile = smfCreate();
	smfSetTimebase(midiFile, MIDI_PPQN); // timebase should be high to make adjusting the song easy.
	const uint64_t midiTicksPerSecond = (float)MIDI_PPQN * ((float)MIDI_BPM / SECONDS_IN_A_MINUTE);
	printf("midiTicksPerSecond: %lu\n", midiTicksPerSecond);
	printf("gbTimeUnitsPerSecond: %u\n", gbTimeUnitsPerSecond);
	midiTicksPerSoundLenTick = round((float)midiTicksPerSecond / 256);
	
	midiTicksPerSecondPointer = &midiTicksPerSecond;
	gbTimeUnitsPerSecondPointer = &gbTimeUnitsPerSecond;
	
	std::vector<std::array<std::pair<uint8_t,bool>, 32>> uniqueWavetables;
	
	//for (int i=0; i<4; i++){
	//	smfInsertControl(midiFile, 0, i, i, SMF_CONTROL_VOLUME, 0); // prevent garbage noise from playing
	//}
		
	gb_chip_state curAPUstate; // whenever a register write is encountered, it will converted to a midi event and then written here. Used to compare the current register write to the previous state.
	
	std::array<uint8_t,4> curPlayingMidiNote = {0xFF, 0xFF, 0xFF, 0xFF}; // The note number of the midi note that is currently playing. One entry for each channel. Used to end the current note, whatever it is. 0xFF means no notes are currently playing.
	std::array<bool,4> legatoState = {false, false, false, false}; // legato mode is turned on whenever the GB does a pitch bend without retriggering the note, but the pitch bend goes beyond the range of a midi note. Legato mode means that when the Plugin is reading back the midi, it should read new notes as pitch changes with no trigger.
	
	uint8_t prevWavetableIndex=0xFF;

	uint64_t midiTicksPassed=0;
	std::array<bool,4> isDACon={true,true,true,true};
	std::array<uint64_t,4> scheduledSoundLenEndTime={0,0,0,0}; // time when a note's sound length should run out in midi ticks (relative to the start of the song)
	for (int regWriteI=0; regWriteI<songData.size(); regWriteI++){
		regWriteIpointer = &regWriteI;
		
		uint16_t registerIndex = songData[regWriteI].address + 0xff00; // TODO: remove " + 0xff00". For now, I'm putting it here for testing; I don't want to rewrite all the case conditions yet.
		uint8_t registerValue = songData[regWriteI].value;
		
		uint8_t regWriteWaveIndex;
		uint64_t regWriteMidiTime = gbTime2midiTime(songData[regWriteI].time, gbTimeUnitsPerSecond, midiTicksPerSecond);
		
		// variables for this switch case.
		std::vector<std::pair<uint8_t, bool>*> propertyVector;
		std::vector<std::pair<uint8_t, uint8_t>> bitRangeVector;
		std::vector<uint8_t> midiCCvector;
		uint8_t channel=0;
		channel = (uint8_t)floor((songData[regWriteI].address - 0x10) / (float)0x5);
		if (channel > 3) channel = 0xFF;
		
		propertyVector = {&(curAPUstate.gb_square1_state.sound_length_enable), &(curAPUstate.gb_square2_state.sound_length_enable), &(curAPUstate.gb_wave_state.sound_length_enable), &(curAPUstate.gb_noise_state.sound_length_enable)};
		for (int i=0; i<4; i++){
			if (scheduledSoundLenEndTime[i] <= regWriteMidiTime && propertyVector[i]->first == true){
				if (curPlayingMidiNote[i]!=0xFF) {
					smfInsertNoteOff(midiFile, regWriteMidiTime, i, i, curPlayingMidiNote[i], 0x7F);
					curPlayingMidiNote[i] = 0xFF;
				}
			}
		}
		
		switch (registerIndex){
			case 0xff10: // square 1
				propertyVector = {&(curAPUstate.gb_square1_state.sweep_speed), &(curAPUstate.gb_square1_state.sweep_up_or_down), &(curAPUstate.gb_square1_state.sweep_shift)};
				bitRangeVector = {std::make_pair(6,4), std::make_pair(3,3), std::make_pair(2,0)};
				midiCCvector = {16, 18, 17};
				handleCommonRegWrite(registerValue, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile); // handles simple regValue -> midi CC conversions
				break;
			case 0xff11:
				handleSqDutyAndSoundLen(registerValue, &(curAPUstate.gb_square1_state), channel, regWriteMidiTime, midiFile); 
				break;
			case 0xff12:
				handleEnv(registerValue, &(curAPUstate.gb_square1_state), channel, regWriteMidiTime, midiFile);
				break;
			case 0xff13:
				//printf("curPlayingMidiNote before: %u\n", curPlayingMidiNote[channel]);
				handlePitchLSB(registerValue, &(curAPUstate.gb_square1_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel]);
				//printf("registerValue == curAPUstate.gb_square1_state.pitchLSB.first: %d\n", registerValue == curAPUstate.gb_square1_state.pitchLSB.first);
				//printf("curPlayingMidiNote after: %u\n", curPlayingMidiNote[channel]);
				break;
			case 0xff14:
				handlePitchMSBtriggerSoundLenEnable(registerValue, &(curAPUstate.gb_square1_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel], scheduledSoundLenEndTime); // skips handling pitchMSB if the channel is noise
				//printf("registerValue & 0b00000111 == curAPUstate.gb_square1_state.pitchMSB.first: %d, %u, %u\n", (registerValue & 0b00000111) == curAPUstate.gb_square1_state.pitchMSB.first, registerValue & 0b00000111, curAPUstate.gb_square1_state.pitchMSB.first);
				break;
			case 0xff16: // square 2
				handleSqDutyAndSoundLen(registerValue, &(curAPUstate.gb_square2_state), channel, regWriteMidiTime, midiFile);
				break;
			case 0xff17:
				handleEnv(registerValue, &(curAPUstate.gb_square2_state), channel, regWriteMidiTime, midiFile);
				break;
			case 0xff18:
				handlePitchLSB(registerValue, &(curAPUstate.gb_square2_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel]);
				break;
			case 0xff19:
				handlePitchMSBtriggerSoundLenEnable(registerValue, &(curAPUstate.gb_square2_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel], scheduledSoundLenEndTime);
				break;
			case 0xff1A: // wave
				{
					uint8_t curWavDAC = extractBitValueFromByte(registerValue, 7, 7);
					if (curAPUstate.gb_wave_state.DAC_off_on.first == 0 && curWavDAC == 1 /* && curAPUstate.gb_wave_state.DAC_off_on.second*/) { // if the DAC was previously off and is now being turned on
						// push curAPUstate.gb_wave_state.wavetable to uniqueWavetables
						if ((std::find(uniqueWavetables.begin(), uniqueWavetables.end(), curAPUstate.gb_wave_state.wavetable.first)) == uniqueWavetables.end()) // element is not in vector
							uniqueWavetables.push_back(curAPUstate.gb_wave_state.wavetable.first);
						// add index of current wave to CC21 at regWriteMidiTime
						uint8_t wavetableIndex = std::distance(std::begin(uniqueWavetables), std::find(uniqueWavetables.begin(), uniqueWavetables.end(), curAPUstate.gb_wave_state.wavetable.first));
						if (wavetableIndex != prevWavetableIndex) {
							smfInsertControl(midiFile, regWriteMidiTime, 2, 2, 21, wavetableIndex); /* if there are more than 127 waves, this will break, but this is unlikely */
							prevWavetableIndex = wavetableIndex;
						}
					}
					curAPUstate.gb_wave_state.DAC_off_on = std::make_pair(curWavDAC, true);
				}
				break;
			case 0xff1B: 
				propertyVector = {&(curAPUstate.gb_wave_state.sound_length)};
				bitRangeVector = {std::make_pair(7,0)};
				midiCCvector = {15};
				handleCommonRegWrite(registerValue, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile);
				break;
			case 0xff1C:
				{
					uint8_t curWaveVol = (registerValue & 0x60) >> 5;
					if (curWaveVol != curAPUstate.gb_wave_state.volume.first || curAPUstate.gb_wave_state.volume.second == false){
						uint8_t midiWaveVol=0;
						switch (curWaveVol){
							case 0:
								midiWaveVol=0;
								break;
							case 0b01:
								midiWaveVol=127;
								break;
							case 0b10:
								midiWaveVol=64;
								break;
							case 0b11:
								midiWaveVol=32;
								break;
							default:
								break;
						}
						smfInsertControl(midiFile, regWriteMidiTime, channel, channel, SMF_CONTROL_VOLUME, midiWaveVol);
					}
					curAPUstate.gb_wave_state.volume = std::make_pair(curWaveVol, true);
				}
				break;
			case 0xff1D:
				handlePitchLSB(registerValue, &(curAPUstate.gb_wave_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel]);
				break;
			case 0xff1E:
				handlePitchMSBtriggerSoundLenEnable(registerValue, &(curAPUstate.gb_wave_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel], scheduledSoundLenEndTime);
				break;
			case 0xff20: // noise
				propertyVector = {&(curAPUstate.gb_noise_state.sound_length)};
				bitRangeVector = {std::make_pair(5,0)};
				midiCCvector = {15};
				handleCommonRegWrite(registerValue, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile);
				break;
			case 0xff21:
				handleEnv(registerValue, &(curAPUstate.gb_noise_state), channel, regWriteMidiTime, midiFile);
				break;
			case 0xff22:
				propertyVector = {&(curAPUstate.gb_noise_state.noise_long_or_short)};
				bitRangeVector = {std::make_pair(3,3)};
				midiCCvector = {20};
				handleCommonRegWrite(registerValue, propertyVector, bitRangeVector, midiCCvector, channel, regWriteMidiTime, midiFile);
				curAPUstate.gb_noise_state.noise_pitch = std::make_pair(registerValue & 0xF7, true); // noise pitch only takes effect when the channel is triggered.
				break;
			case 0xff23:
				handlePitchMSBtriggerSoundLenEnable(registerValue, &(curAPUstate.gb_noise_state), channel, regWriteMidiTime, midiFile, curPlayingMidiNote, legatoState[channel], scheduledSoundLenEndTime);
				break;
			case 0xff25: // control
				handlePanning(&(curAPUstate), registerValue, midiFile, regWriteMidiTime);
				break;
			case 0xff30: // wave table
			case 0xff31:
			case 0xff32:
			case 0xff33:
			case 0xff34:
			case 0xff35:
			case 0xff36:
			case 0xff37:
			case 0xff38:
			case 0xff39:
			case 0xff3A:
			case 0xff3B:
			case 0xff3C:
			case 0xff3D:
			case 0xff3E:
			case 0xff3F:
				if (curAPUstate.gb_wave_state.DAC_off_on.first == 0 /*&& curAPUstate.gb_wave_state.DAC_off_on.second*/) {
					regWriteWaveIndex = (uint8_t)((registerIndex - 0xff30)*2);
					curAPUstate.gb_wave_state.wavetable.first[regWriteWaveIndex].first = (registerValue & 0xF0) >> 4;
					curAPUstate.gb_wave_state.wavetable.first[regWriteWaveIndex].second = true;
					curAPUstate.gb_wave_state.wavetable.first[regWriteWaveIndex+1].first = registerValue & 0xF;
					curAPUstate.gb_wave_state.wavetable.first[regWriteWaveIndex+1].second = true;
				}
				break;
			default:
				break;
		}
		if (regWriteMidiTime > midiTicksPassed) midiTicksPassed = regWriteMidiTime;
	}
	// add wavetables to midi.
	unsigned int sysexDataSize = 2 /* start and end bytes */ + 32 * uniqueWavetables.size();
	uint8_t sysexData[sysexDataSize];
	sysexData[0]=0xF0;
	unsigned int sysexWaveIndex=0;
	unsigned int sysexDataIndex=0;
	for (std::array<std::pair<uint8_t,bool>, 32> curWavetable : uniqueWavetables) {
		sysexDataIndex = 1+sysexWaveIndex*32;
		if (sysexDataIndex >= sysexDataSize) {fprintf(stderr, "out of range (1)! %u >= %u\n", sysexDataIndex, sysexDataSize);}
		for (int i=0; i<32; i++){
			sysexDataIndex = 1+sysexWaveIndex*32+i;
			if (sysexDataIndex >= sysexDataSize) {fprintf(stderr, "out of range (2)! %u >= %u\n", sysexDataIndex, sysexDataSize);}
			sysexData[sysexDataIndex] = curWavetable[i].first & 0x0F; // DO NOT convert wave back to gb format. leave each 4-bit sample in its own byte so that the data can never accidentally match the sysex end byte 0xF7
		}
		sysexWaveIndex++;
	}
	sysexData[sysexDataSize-1]=0xF7;
	smfInsertSysex(midiFile, 0 /* time */, 0 /* port */, 2 /* wave track */, sysexData, sysexDataSize);
	
	/*
	for (std::array<uint8_t,32> curWavetable : uniqueWavetables) {
		for (int i=0; i<32; i++){
			printf("%02X ", curWavetable[i]);
		}
		printf("\n");
	}
	*/
	
	smfSetEndTimingOfTrack(midiFile, 0, midiTicksPassed);
	smfSetEndTimingOfTrack(midiFile, 1, midiTicksPassed);
	smfSetEndTimingOfTrack(midiFile, 2, midiTicksPassed);
	smfSetEndTimingOfTrack(midiFile, 3, midiTicksPassed);
	smfWriteFile(midiFile, outfilename.c_str());
	
	auto stop = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
	printf("songData2midi: %ld milliseconds.\n", duration.count());
	return true;
}
