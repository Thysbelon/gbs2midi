/*
This file contains the code that converts gbsplay's output to gb_reg_write structs.
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

#include "gb_reg_write.h"

#include "from_gbsplay.hpp"

bool gbsplayStdout2songData(std::vector<gb_reg_write>& songData, std::string gbsFileName, int subsongNum, int timeInSeconds){
auto start = std::chrono::high_resolution_clock::now();

#ifdef WIN32
std::string progPrefix = ".\\";
std::string progSuffix = ".exe";
#else
std::string progPrefix = "./";
std::string progSuffix = "";
#endif
std::string gbsplayCmd = progPrefix+"gbsplay"+progSuffix+" -t "+ std::to_string(timeInSeconds) +" -o iodumper -- "+gbsFileName+" "+std::to_string(subsongNum)+" "+std::to_string(subsongNum);
printf("DEBUG: going to call popen(%s)\n", gbsplayCmd.c_str());
FILE *gbsplayFile = popen(gbsplayCmd.c_str(), "r"); // https://stackoverflow.com/questions/125828/capturing-stdout-from-a-system-command-optimally
char line[1024];
uint64_t cyclesPassed=0;
gb_reg_write curRegWrite;
for (int i=0; i<2; i++){ // skip 2 lines
	fgets(line, sizeof(line), gbsplayFile);
}
while (fgets(line, sizeof(line), gbsplayFile)){ 
	// read cycleDiff, registerIndex, and registerValue from each line.
	uint32_t cycleDiff = std::stoul(std::string(line).substr(0, 8), nullptr, 16); // https://stackoverflow.com/questions/1070497/c-convert-hex-string-to-signed-integer
	uint16_t registerIndex = std::stoul(std::string(line).substr(9, 4), nullptr, 16);
	uint8_t registerValue = std::stoul(std::string(line).substr(14, 2), nullptr, 16);
	
	curRegWrite.address = registerIndex & 0xFF; // remove 0xFF00 from every registerIndex to save space. curRegWrite.address is relative to 0xFF00 in GB memory.
	curRegWrite.value = registerValue;
	
	// add the value of cycleDiff to cyclesPassed on each line.
	cyclesPassed += cycleDiff;
	
	curRegWrite.time = cyclesPassed;
	//curRegWrite.time = cycleDiff;

	songData.push_back(curRegWrite);
	curRegWrite = gb_reg_write{};
	
	//printf("cycleDiff: 0x%08x, registerIndex: 0x%04x, registerValue: 0x%02x\n", cycleDiff, registerIndex, registerValue);
}

/*
for (gb_reg_write i: songData){
	printf("time: 0x%016lx, address: 0x%02x, value: 0x%02x\n", i.time, i.address, i.value); // when stdout is being printed to the commmand line, not all of these will display. Redirect this command's output to a text file to see everything.
}
*/

auto stop = std::chrono::high_resolution_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
printf("gbsplayStdout2songData: %ld milliseconds.\n", duration.count());
return true;
}