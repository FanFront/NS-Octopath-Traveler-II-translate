#include "../graphics/backend.h"
#include "../thread_services.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" void ot2_host_log(const char* format,...){if(!getenv("OT2_TEST_VERBOSE"))return;va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc('\n',stderr);}
extern "C" void ot2_host_die(const char* format,...){va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc('\n',stderr);abort();}
extern "C" void* ot2_native_gate(const char*,void* function){return function;}
extern "C" uint64_t ot2_call_guest_address(uint64_t address,const uint64_t* args){
    using Callback=void(*)(NVNcommandBuffer*,NVNcommandBufferMemoryEvent,size_t,void*);
    reinterpret_cast<Callback>(address)(reinterpret_cast<NVNcommandBuffer*>(args[0]),NVNcommandBufferMemoryEvent(args[1]),args[2],reinterpret_cast<void*>(args[3]));return 0;
}
#ifdef OT2_FRONTEND_ONLY
// CPU-only tests exercise the actual frontend and inspect its owned records.
// Android builds use the real Vulkan backend and test submitted pixel results.
extern "C" int vkhost_device_limit(NVNdeviceInfo,int maximum){return maximum;}
extern "C" void vkhost_pool_add(const void*,uint64_t){}
extern "C" void vkhost_pool_del(const void*){}
extern "C" void vkhost_descriptor_pool_del(const dpool_t*,int){}
extern "C" void vkhost_reg_tex(const dpool_t*,int,const tex_t*,const view_t*){}
extern "C" void vkhost_reg_samp(const dpool_t*,int,const samp_par*){}
extern "C" void vkhost_set_tex_pool_n(int){}
extern "C" void vkhost_tex_created(const tex_t*){}
extern "C" void vkhost_tex_release(tex_t*){}
extern "C" void vkhost_tex_retain(uint64_t){}
extern "C" void vkhost_tex_unref(uint64_t){}
extern "C" void vkhost_prog_release(prog_t*){}
extern "C" void vkhost_program(const prog_t*){}
extern "C" void vkhost_write_texels(const tex_t*,const view_t*,const NVNcopyRegion*,const void*,int64_t,int64_t){}
extern "C" void vkhost_read_texels(const tex_t*,unsigned,unsigned,const NVNcopyRegion*,uint8_t*,size_t,size_t){}
extern "C" uint64_t vkhost_last_fence(){return 0;}
extern "C" int vkhost_wait(uint64_t,uint64_t){return 1;}
extern "C" void vkhost_finish(){}
extern "C" void vkhost_submit(int,const uint64_t*){}
extern "C" uint64_t vkhost_present(const tex_t*,int,const int*){return 0;}
#endif
