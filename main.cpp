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

#include "from_gbsplay.hpp"
#include "to_midi.hpp"
#include "gb_reg_write.h"

const uint32_t MASTER_CLOCK = 0x400000; // game boy cycles per second. 4194304

int main(int argc, char *const argv[]){
	
if (argc<4) {
	printf("./gbs2midi file.gbs subsongNumber outfile.mid [timeInSeconds] \n");
	return 1;
}

// the song "Big Forest" from Kirby's Dream Land 2 functions strangely. When played via gbsplay, during the intro, square 1 is muted, and this seems to happen because square 1's panning is set to 0 0. However, emulators and real hardware will play square 1: https://www.youtube.com/watch?v=e2_Ly1cBMR4
	
// songData is a list of register writes pulled directly from gbsplay (or other source like vgm file)
std::vector<gb_reg_write> songData;

std::string outfilename = std::string(argv[3]);

if (outfilename.substr(outfilename.length()-4, 4) == ".mid") {
	gbsplayStdout2songData(songData, std::string(argv[1]), atoi(argv[2]), argc >= 5 ? atoi(argv[4]) : 150);
	unsigned int gbTimeUnitsPerSecond = MASTER_CLOCK; // TODO: implement vgm2songData conversion. For this variable to the left, use 0x400000 for gbsplay and 44100 for vgm.
	songData2midi(songData, gbTimeUnitsPerSecond, outfilename);
} else {
	printf("Valid output file extensions are .mid\n");
}

return 0;
}