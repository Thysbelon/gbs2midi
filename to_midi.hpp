#pragma once

#include <string>
#include <vector>

#include "gb_reg_write.h"

bool songData2midi(std::vector<gb_reg_write>& songData, unsigned int gbTimeUnitsPerSecond, std::string outfilename, int inPPQN);