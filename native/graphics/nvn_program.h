#pragma once
#include "nvn_obj.h"
#ifdef __cplusplus
#include <array>
#include <memory>
#include <string>
#include <vector>
struct NvnProgramVersion {
    prog_t value{};
    std::array<std::array<uint8_t,0x880>,6> controls{};
    std::array<std::vector<uint8_t>,6> code;
    std::array<std::string,6> keys;
    std::array<int,6> stages{-1,-1,-1,-1,-1,-1};
    explicit NvnProgramVersion(const prog_t&);
};
std::shared_ptr<const NvnProgramVersion> nvn_program_get(uint64_t serial);
extern "C" {
#endif
void nvn_program_publish(prog_t*);
void nvn_program_release(uint64_t serial);
#ifdef __cplusplus
}
#endif
