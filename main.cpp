#include <cstdint>
#include <string>
#include <cstdio>
#include <unistd.h> // access

#include "from_gbsplay.hpp"
#include "to_midi.hpp"
#include "gb_reg_write.h"

const uint32_t MASTER_CLOCK = 0x400000; // game boy cycles per second. 4194304

enum errorCode
{
	NOERROR,
	NOT_ENOUGH_ARGS, // errors in main.cpp
	INPUT_NOT_FOUND,
	INVALID_OUTPUT_TYPE,
	INVALID_INPUT_TYPE,
	NO_GBSPLAY
};

void displayHelp(){
	printf("How to use: \n./gbs2midi file.gbs subsongNumber outfile.mid [Midi_ticks_per_quarter_note] [timeInSeconds] \n");
}

bool exists(const std::string& name) { // https://stackoverflow.com/questions/12774207/fastest-way-to-check-if-a-file-exists-using-standard-c-c11-14-17-c
	return ( access( name.c_str(), F_OK ) != -1 );
}

int main(int argc, char *const argv[]){
	
if (argc<4) {
	displayHelp();
	return NOT_ENOUGH_ARGS;
}
std::string inFilename = std::string(argv[1]);
if (exists(inFilename) == false) {
	fprintf(stderr, "Error: Input filename does not exist.\n");
	return INPUT_NOT_FOUND;
}
int subsongNumber = atoi(argv[2]);
if (subsongNumber < 1) {
	fprintf(stderr, "Warning: Subsong Number was set to a number less than 1. Forcing subsong number to 1...\n");
	subsongNumber=1;
}
std::string outfilename = std::string(argv[3]);
if (outfilename.substr(outfilename.length()-4, 4) != ".mid") {
	fprintf(stderr, "Error: The only valid output file extension is .mid (in all lowercase).\n");
	return INVALID_OUTPUT_TYPE;
}
int PPQN = argc >= 5 ? atoi(argv[4]) : 0x7fff;
if (PPQN < 1) {
	fprintf(stderr, "Warning: Midi_ticks_per_quarter_note was set to a value less than 1. Forcing to 0x7fff...\n");
	PPQN=0x7fff;
}
int timeInSeconds = argc >= 6 ? atoi(argv[5]) : 150;
if (timeInSeconds < 1) {
	fprintf(stderr, "Warning: Time was set to a value less than 1 second. Forcing time to 150 seconds...\n");
	timeInSeconds=150;
}

// the song "Big Forest" from Kirby's Dream Land 2 functions strangely. When played via gbsplay, during the intro, square 1 is muted, and this seems to happen because square 1's panning is set to 0 0. However, emulators and real hardware will play square 1: https://www.youtube.com/watch?v=e2_Ly1cBMR4
	
// songData is a list of register writes pulled directly from gbsplay (or other source like vgm file)
std::vector<gb_reg_write> songData;

unsigned int gbTimeUnitsPerSecond;
if (inFilename.substr(inFilename.length()-4, 4) == ".gbs" || inFilename.substr(inFilename.length()-4, 4) == ".GBS") {
#ifdef WIN32
	if (exists("gbsplay.exe") == false)
#else
	if (exists("gbsplay") == false) 
#endif
	{
		fprintf(stderr, "Error: gbsplay executable does not exist in this directory.\n");
		return NO_GBSPLAY;
	}
	gbsplayStdout2songData(songData, inFilename, subsongNumber, timeInSeconds);
	gbTimeUnitsPerSecond = MASTER_CLOCK; // TODO: implement vgm2songData conversion. For this variable to the left, use 0x400000 for gbsplay and 44100 for vgm.
	//printf("gbTimeUnitsPerSecond: %u\n", gbTimeUnitsPerSecond); // redundant
} else {
	fprintf(stderr, "Error: Currently, the only valid input file extension is .gbs (in all lowercase, or in all uppercase).\n");
	if(inFilename.substr(inFilename.length()-4, 4) == ".vgm" || inFilename.substr(inFilename.length()-4, 4) == ".VGM") 
		fprintf(stderr, "VGM support has not been added. If you would like me to add VGM support, please open an issue on the gbs2midi GitHub repository.\n");
	return INVALID_INPUT_TYPE;
}
songData2midi(songData, gbTimeUnitsPerSecond, outfilename, PPQN);

return NOERROR;
}