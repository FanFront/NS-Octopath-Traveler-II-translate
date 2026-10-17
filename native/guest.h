#pragma once
#include "loader.h"
#include <cstdint>
#include <string>
namespace ot2 {
std::string execution_probe();
void boot_game(const std::string& directory, const std::string& log_path,uint64_t program_id);
void set_frame_rate_target(int fps);
int frame_rate_applied();
void freeze_guest_clock(unsigned reason,bool on);
}
