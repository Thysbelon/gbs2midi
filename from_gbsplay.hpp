#pragma once

#include <string>
#include <vector>

#include "gb_reg_write.h"

bool gbsplayStdout2songData(std::vector<gb_reg_write>& songData, std::string gbsFileName, int subsongNum, int timeInSeconds = 150);