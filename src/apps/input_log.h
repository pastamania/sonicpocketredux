// The button log format the tools share: BizHawk's, "<frame>\t<Button,Button,...>"
// per line, also what `sonic-pocket --record` writes.
#pragma once

#include "ngpc/machine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

inline std::map<int, uint8_t> read_input_log(const std::filesystem::path &path)
{
	std::map<int, uint8_t> inputs;
	std::ifstream file(path);
	std::string line;
	int index = 0;
	while (std::getline(file, line))
	{
		const auto tab = line.find('\t');
		const std::string names = tab == std::string::npos ? "" : line.substr(tab + 1);
		uint8_t buttons = 0;
		std::stringstream stream(names);
		std::string name;
		while (std::getline(stream, name, ','))
		{
			if (name.find("Up") != std::string::npos) buttons |= ngpc::BUTTON_UP;
			else if (name.find("Down") != std::string::npos) buttons |= ngpc::BUTTON_DOWN;
			else if (name.find("Left") != std::string::npos) buttons |= ngpc::BUTTON_LEFT;
			else if (name.find("Right") != std::string::npos) buttons |= ngpc::BUTTON_RIGHT;
			else if (name.find("Option") != std::string::npos) buttons |= ngpc::BUTTON_OPTION;
			else if (name.size() >= 1 && name.back() == 'A') buttons |= ngpc::BUTTON_A;
			else if (name.size() >= 1 && name.back() == 'B') buttons |= ngpc::BUTTON_B;
		}
		inputs[index++] = buttons;
	}
	return inputs;
}
