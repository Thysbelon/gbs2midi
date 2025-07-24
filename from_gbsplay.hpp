#pragma once

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

#include "gb_reg_write.h"

bool gbsplayStdout2songData(std::vector<gb_reg_write>& songData, std::string gbsFileName, int subsongNum, int timeInSeconds = 150);