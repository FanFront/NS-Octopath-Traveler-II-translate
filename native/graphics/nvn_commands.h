#pragma once
#include "nvn_obj.h"
#ifdef __cplusplus
extern "C" {
#endif
// Host-only ownership; guest texture/program ABIs never grow. Grants retain
// recorded object versions until their memory is re-granted or its pool dies.
void nvn_commands_grant(cmd_t*,void* memory,size_t size);
void nvn_commands_finalize(cmd_t*);
void nvn_commands_pool_release(const void* memory,size_t size);
const tex_t* nvn_command_texture(cmd_t*,const tex_t*);
const prog_t* nvn_command_program(cmd_t*,const prog_t*);
const dpool_t* nvn_command_pool(cmd_t*,const dpool_t*);
void nvn_commands_copy_record(cmd_t*,op_hdr*);
// A queued submission pins every reached segment (including Call/JUMP).
// Release only after translation/GPU no longer borrows the recorded objects.
void* nvn_commands_pin(int n,const uint64_t* handles);
void nvn_commands_unpin(void*);
void nvn_commands_stats(size_t* segments,size_t* textures,size_t* programs);
void nvn_commands_publish_fences(int n,const uint64_t* handles,uint64_t sequence);
#ifdef __cplusplus
}
#include <vector>
std::vector<uint64_t> nvn_commands_waits(int count,const uint64_t* handles);
// Optional bounded expansion over pinned memory only. Empty means fall back
// to ordinary replay/limited lookahead, never skip the actual command stream.
std::vector<const uint8_t*> nvn_commands_index(void* pins,int count,const uint64_t* handles);
#endif
