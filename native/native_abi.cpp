#include "native_abi.h"
#include "graphics/nvn_protos.h"
#include <cstring>

unsigned native_register_abi(const char* name){
    // Presentation owns frame policy and instrumentation; retain its existing
    // entry. Unknown or nonstandard prototypes always retain the generic ABI.
    if(!strcmp(name,"nvnQueuePresentTexture"))return 0;
    struct Entry {const char* name;unsigned abi;};
    static constexpr Entry entries[]={
#define NVN_ABI(name) {#name,NativeAbi<NVNT_##name>::value},
#include "nvn_abi_entries.inc"
#undef NVN_ABI
    };
    for(auto& e:entries)if(!strcmp(name,e.name))return e.abi;
    return 0;
}
