#include "guest.h"
#include "fault_decode.h"
#include "guest_clock.h"
#include "frame_rate_patch.h"
#include "guest_context.h"
#include "native_abi.h"
#include "thread_services.h"
#include "host_services.h"
#include "graphics/nvn_dirty.h"
#include <asm/sigcontext.h>
#include "graphics/backend.h"
#include <android/log.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

extern "C" {
alignas(4) __attribute__((visibility("hidden"))) uint32_t ot2_register_gates_enabled=0;
alignas(4) __attribute__((visibility("hidden"))) uint32_t ot2_native_abis[2048]{};
void ot2_signal_entry(int, siginfo_t*, void*);
uint64_t ot2_guest_call(uintptr_t, GuestThread*, const uint64_t*, uintptr_t);
void ot2_host_invoke(void*,uint64_t*,void*,uintptr_t);
void ot2_abi_fixture();
void ot2_native_entry();
uint64_t ot2_host_stack_call(uint64_t (*)(uint64_t*),uint64_t*,uintptr_t);
}
namespace {
struct Trap { uintptr_t pc; uint32_t instruction; const char* hook; uint64_t (*call)(uint64_t*)=nullptr; uint64_t hits=0; };
std::vector<Trap> traps;
alignas(4) uint32_t fast_tls_enabled=1;
std::atomic<uint64_t> tls_signal_count{0};
std::atomic<uint64_t> svc_signal_count{0},counter_signal_count{0},sdk_signal_count{0};
struct NativeGate {char name[160];void* function;uint64_t hits;bool present;};
NativeGate native_gates[2048]{};
std::atomic<unsigned> native_gate_count{0};
uint8_t* native_gate_code=nullptr;
std::mutex native_gate_mutex;
std::vector<ot2::ModuleView> images;
int log_fd=-1;
uint8_t* address_space{};
constexpr size_t reserve_size=128ULL<<30;
constexpr size_t alias_offset=0x10000000, alias_size=96ULL<<30;
constexpr size_t stack_offset=100ULL<<30, stack_capacity=512ULL<<20;
constexpr size_t heap_offset=101ULL<<30, heap_capacity=4ULL<<30;
size_t heap_size=0;
uint64_t image_size=0;
struct Mapping { uintptr_t base; size_t size; uint32_t state; uintptr_t source; };
std::vector<Mapping> mappings;
std::mutex mapping_mutex;
std::mutex setup_mutex;
bool thread_system_ready=false;
ot2::LoadedGame* active_game=nullptr;
thread_local ucontext_t* active_signal=nullptr;
thread_local uintptr_t service_callback_stack=0;
thread_local unsigned host_service_depth=0;

void log(const char* fmt, ...) {
    char buffer[2048]; va_list args; va_start(args,fmt);
    vsnprintf(buffer,sizeof(buffer),fmt,args); va_end(args);
    __android_log_write(ANDROID_LOG_INFO,"OT2Guest",buffer);
    if(log_fd>=0) { write(log_fd,buffer,strlen(buffer)); write(log_fd,"\n",1); }
}
[[noreturn]] void fail(const char* reason, const ucontext_t* uc=nullptr) {
    log("STOP %s",reason);
    if(uc) {
        const auto& c=uc->uc_mcontext;
        log("pc=%llx lr=%llx sp=%llx",c.pc,c.regs[30],c.sp);
        for(const auto& m:images) {
            auto b=reinterpret_cast<uintptr_t>(m.base);
            if(c.pc>=b && c.pc<b+m.size) log("PC %s+0x%llx",m.name.c_str(),c.pc-b);
            if(c.regs[30]>=b && c.regs[30]<b+m.size) log("LR %s+0x%llx",m.name.c_str(),c.regs[30]-b);
        }
        for(unsigned i=0;i<31;i+=3) log("x%u=%llx x%u=%llx x%u=%llx",i,c.regs[i],i+1,i+1<31?c.regs[i+1]:0,i+2,i+2<31?c.regs[i+2]:0);
        uintptr_t fp=c.regs[29];
        for(unsigned depth=0;depth<24 && fp;depth++) {
            bool readable=false;
            for(unsigned n=0;n<GuestThreadCapacity;n++) {
                const auto& t=ot2_threads[n];
                if(fp>=t.stack && fp+16<=t.stack+t.stack_size) readable=true;
            }
            if(!readable || (fp&15)) break;
            const auto* frame=reinterpret_cast<const uintptr_t*>(fp);
            for(const auto& m:images) {
                auto base=reinterpret_cast<uintptr_t>(m.base);
                if(frame[1]>=base && frame[1]<base+m.size) log("FRAME %u %s+0x%llx",depth,m.name.c_str(),(unsigned long long)(frame[1]-base));
            }
            if(frame[0]<=fp) break;
            fp=frame[0];
        }
    }
    if(log_fd>=0) fsync(log_fd);
    _exit(71);
}
GuestThread* current() { return ot2_guest_context_current(); }
ot2::GuestClock guest_clock;
uint64_t monotonic_ns() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000000000 + uint64_t(ts.tv_nsec);
}
uint64_t ticks() {
    const auto ns=guest_clock.read(monotonic_ns);
    return (ns/1000000000)*19200000 + (ns%1000000000)*12/625;
}
GuestThread* create_thread_context() { return ot2_guest_context_create(); }
void install_handlers() {
    struct sigaction action{}; action.sa_sigaction=ot2_signal_entry;
    action.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER; sigemptyset(&action.sa_mask);
    for(int sig:{SIGTRAP,SIGSEGV,SIGBUS,SIGILL,SIGFPE}) if(sigaction(sig,&action,nullptr)) fail("Signal handler setup failed");
}
bool special(uint32_t instruction) {
    auto op=instruction&~31U;
    return (instruction&0xffe0001f)==0xd4000001 || op==0xd53bd040 || op==0xd53bd060 ||
        op==0xd51bd040 || op==0xd53be020 || op==0xd53be000;
}
#include "tls_rewrite.inc"
#include "system_rewrite.inc"
void patch(uint8_t* base,size_t size) {
    const auto first_island=tls_islands.size();
    size_t tls_native=0,tls_fallback=0;
    for(size_t offset=0;offset+4<=size;offset+=4) {
        uint32_t op; memcpy(&op,base+offset,4);
        if(!special(op)) continue;
        if(patch_tls(reinterpret_cast<uintptr_t>(base+offset),op,first_island)){++tls_native;continue;}
        if(patch_system(reinterpret_cast<uintptr_t>(base+offset),op,first_island))continue;
        auto kind=op&~31u;if(kind==0xd53bd040||kind==0xd53bd060||kind==0xd51bd040)++tls_fallback;
        traps.push_back({reinterpret_cast<uintptr_t>(base+offset),op,nullptr});
        const uint32_t brk=0xd4200000 | (0x702<<5);
        memcpy(base+offset,&brk,4);
    }
    seal_tls_islands(first_island);
    if(size>4096)log("TLS_PATCH base=%p bytes=%zu native=%zu signal=%zu islands=%zu",base,size,tls_native,tls_fallback,tls_islands.size()-first_island);
}
void install_service_initializers(const ot2::LoadedGame& game) {
    const auto first_island=tls_islands.size();
    // This game's own SDK ships its direct-call audio renderer. Select it at
    // the manager seam instead of opening Horizon's audren IPC service.
    const auto audio_ipc=game.symbol("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv");
    const auto audio_dfc=game.symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if(!audio_ipc || !audio_dfc || (int64_t(audio_dfc)-int64_t(audio_ipc))%4 ||
       std::abs(int64_t(audio_dfc)-int64_t(audio_ipc))>=(1LL<<27))fail("SDK direct audio entry unavailable");
    const uint32_t audio_branch=0x14000000U | (uint32_t((int64_t(audio_dfc)-int64_t(audio_ipc))/4)&0x03ffffffU);
    memcpy(reinterpret_cast<void*>(audio_ipc),&audio_branch,4);
    // Service clients are replaced by host APIs, so their Horizon IPC sessions
    // must not be created. Keep nn::os and libc initialization in the original SDK.
    static constexpr const char* names[]={
        "nndiagStartup", "_ZN2nn2oe10InitializeEv", "_ZN2nn3aoc10InitializeEv",
        "_ZN2nn3err26InitializeContextWriterApiEv", "_ZN2nn2fs24InitializeForApplicationEv",
        "_ZN2nn4pctl10InitializeEv", "_ZN2nn4diag34InitializeApplicationAbortObserverEv"
    };
    for(const auto* name:names) {
        const auto address=game.symbol(name);
        if(!address) fail("Service initialization export absent");
        uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
        if(patch_system(address,original,first_island,name))continue;
        traps.push_back({address,original,name});
        const uint32_t brk=0xd4200000 | (0x703<<5);
        memcpy(reinterpret_cast<void*>(address),&brk,4);
    }
    for(const auto* list:{&ot2::host_services(),&ot2::audio_services(),&ot2::input_services(),&ot2::movie_services()}) for(const auto& service:*list) {
        const auto address=game.symbol(service.name);
        if(!address) continue;
        std::erase_if(traps,[&](const auto& t){return t.pc==address;});
        uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
        if(patch_system(address,original,first_island,service.name,service.call))continue;
        traps.push_back({address,original,service.name,service.call});
        const uint32_t brk=0xd4200000 | (0x703<<5);
        memcpy(reinterpret_cast<void*>(address),&brk,4);
    }
    seal_tls_islands(first_island);
}
#include "guest_memory.inc"
std::atomic<int> requested_frame_rate{30},applied_frame_rate{0};
ot2::FrameRatePatch frame_rate_patch;
void update_frame_rate(){
    struct Memory {
        void* resolve(uintptr_t address,size_t length,bool write){
            if(!length||address>UINTPTR_MAX-length)return nullptr;
            const int need=PROT_READ|(write?PROT_WRITE:0);
            // All fields fit eight bytes; check both 4-KiB guest pages if a
            // pointer straddles the boundary, including borrowed source pages.
            if((memory_permission_at(address)&need)!=need||(memory_permission_at(address+length-1)&need)!=need)return nullptr;
            return reinterpret_cast<void*>(address);
        }
    } memory;
    std::lock_guard lock(mapping_mutex);
    const int target=requested_frame_rate.load(std::memory_order_relaxed);
    auto result=frame_rate_patch.apply(memory,target);
    if(result==ot2::FrameRatePatch::changed||result==ot2::FrameRatePatch::unchanged){
        if(applied_frame_rate.exchange(target)!=target)log("FRAME_RATE_POLICY applied=%d build=0d9649011312f62e clock=unchanged",target);
    }else if(applied_frame_rate.exchange(0)!=0)log("FRAME_RATE_POLICY unavailable result=%d",int(result));
}
void svc(unsigned id,ucontext_t* uc,GuestThread* c) {
    auto* r=uc->uc_mcontext.regs;
    static bool seen[256]{};
    if(id<256 && !seen[id]) { seen[id]=true; log("SVC 0x%x x0=%llx x1=%llx x2=%llx x3=%llx",id,r[0],r[1],r[2],r[3]); }
    if(id==0x1e) { r[0]=ticks(); return; }
    if(id==0x15 || id==0x51 || id==0x52) {
        std::lock_guard lock(mapping_mutex);
        const uintptr_t address=r[1];const size_t length=r[2];const uint32_t handle=r[0],permission=r[3];
        uint64_t transfer_serial=0;
        if(auto result=memory_validate(address,length,memory_invalid_current)){r[0]=result;return;}
        if(id!=0x15&&!memory_contains(reinterpret_cast<uintptr_t>(address_space),reserve_size,address,length)){r[0]=memory_invalid_current;return;}
        if(id!=0x52&&permission!=0&&permission!=1&&permission!=3){r[0]=0xd801;return;}
        if(id==0x15){
            if(!memory_source_available(address,length)){r[0]=memory_invalid_current;return;}
            transfer_sources.reserve(transfer_sources.size()+1);
        }else{
            auto owner=std::find_if(transfer_sources.begin(),transfer_sources.end(),[&](const auto& t){return t.handle==handle&&!t.closed;});
            if(owner==transfer_sources.end()){r[0]=0xe401;return;}
            transfer_serial=owner->serial;
            const auto base=reinterpret_cast<uintptr_t>(address_space);
            if(memory_overlaps(address,length,base+heap_offset,heap_capacity)||memory_overlaps(address,length,base+alias_offset,alias_size)){r[0]=memory_invalid_region;return;}
            if(owner->size!=length){r[0]=memory_invalid_size;return;}
            if(id==0x51){
                if(owner->permission!=permission||std::any_of(mappings.begin(),mappings.end(),[&](const auto& m){return (m.state==0xd||m.state==0xe)&&m.source==transfer_serial;})){r[0]=0xfa01;return;}
                if(!memory_destination_free(address,length)){r[0]=memory_invalid_current;return;}
                mappings.reserve(mappings.size()+1);
            }else{
                auto mapped=std::find_if(mappings.begin(),mappings.end(),[&](const auto& m){return m.base==address&&m.size==length&&(m.state==0xd||m.state==0xe)&&m.source==transfer_serial;});
                if(mapped==mappings.end()){r[0]=memory_invalid_current;return;}
            }
        }
        if(!svc_thread(id,reinterpret_cast<uint64_t*>(r)))fail("Transfer memory adapter absent",uc);
        if(!r[0])memory_registry_changed();
        if(!r[0]&&id==0x15){
            transfer_sources.push_back({address,length,uint32_t(r[1]),permission,false,++next_transfer_serial});memory_protect(address,length,permission);
        }
        if(!r[0]&&id==0x51)mappings.push_back({address,length,permission?0xeu:0xdu,transfer_serial});
        if(!r[0]&&id==0x52)std::erase_if(mappings,[&](const auto& m){return m.base==address&&m.size==length&&(m.state==0xd||m.state==0xe)&&m.source==transfer_serial;});
        return;
    }
    if(id==0x16){
        std::lock_guard lock(mapping_mutex);const auto handle=uint32_t(r[0]);
        auto owner=std::find_if(transfer_sources.begin(),transfer_sources.end(),[&](const auto& t){return t.handle==handle&&!t.closed;});
        if(owner!=transfer_sources.end()){
            const bool mapped=std::any_of(mappings.begin(),mappings.end(),[&](const auto& m){return (m.state==0xd||m.state==0xe)&&m.source==owner->serial;});
            if(!svc_thread(id,reinterpret_cast<uint64_t*>(r)))fail("Transfer close adapter absent",uc);
            if(!r[0]){
                if(mapped)owner->closed=true;
                else {const auto address=owner->base,length=owner->size;transfer_sources.erase(owner);memory_protect(address,length,PROT_READ|PROT_WRITE);}
            }
            return;
        }
    }
    if(thread_system_ready && svc_thread(id,reinterpret_cast<uint64_t*>(r))) return;
    if(id==0x10) { r[0]=0; return; }
    if(id==0x25) { r[0]=0;r[1]=c->handle;return; }
    if(id==0x0c) { r[0]=0;r[1]=44;return; }
    if(id==0x0e) { r[0]=0;r[1]=0;r[2]=7;return; }
    if(id==0x27) { log("GUEST %.*s",int(std::min<uint64_t>(r[1],1600)),reinterpret_cast<char*>(r[0]));r[0]=0;return; }
    if(id==0x29) {
        std::lock_guard lock(mapping_mutex);
        auto base=reinterpret_cast<uintptr_t>(address_space); uint64_t value=0;
        switch(r[1]) {
        case 0:value=7;break; case 1:value=0xffffffffffffffffULL;break;
        case 2:value=base+alias_offset;break; case 3:value=alias_size;break;
        case 4:value=base+heap_offset;break; case 5:value=heap_capacity;break;
        case 6:case 21:value=0xcd500000;break;
        case 7:case 22:value=heap_size+image_size;break;
        case 8:value=0;break;
        case 11:if(getrandom(&value,8,0)!=8) fail("Random source failed",uc);break;
        case 12:value=base;break; case 13:value=reserve_size;break;
        case 14:value=base+stack_offset;break; case 15:value=stack_capacity;break;
        case 16:value=0x2000000;break; case 17:value=0x400000;break;
        case 18:value=ot2::configured_program_id();break; case 20:value=0;break;
        case 23:value=1;break;
        case 28:
            if(r[3]){r[0]=0xf001;return;} // Kernel InvalidEnumValue
            if(uint32_t(r[2])!=0xffff8001U){r[0]=0xe401;return;} // Current process handle only
            // The host reserves a fixed alias region (id 3); it does not add
            // the optional extra region selected by newer Horizon processes.
            // Report that actual layout, not an invented per-game size.
            value=0;break;
        default:log("Unhandled GetInfo id=%llu",r[1]);fail("GetInfo unavailable",uc);
        }
        r[0]=0;r[1]=value;return;
    }
    if(id==0x06) {
        std::lock_guard lock(mapping_mutex);
        const uintptr_t address=r[2]; uint64_t b=0,size=0; uint32_t state=0,permission=0,attribute=0;
        if(address>=c->stack && address<c->stack+c->stack_size) { b=c->stack;size=c->stack_size;state=0xb;permission=3; }
        else if(const auto image=memory_image_region(address);image.size){b=image.base;size=image.size;state=image.state;permission=image.permission;}
        if(!size) for(const auto& m:mappings) if(address>=m.base && address<m.base+m.size) {b=m.base;size=m.size;state=m.state;permission=3;break;}
        const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
        if(!size && address>=heap && address<heap+heap_size) {b=heap;size=heap_size;state=5;permission=3;}
        memory_query_source(address,b,size,state,attribute,permission);
        if(!size && address>=(1ULL<<39)){b=1ULL<<39;size=0-b;state=0x10;permission=0;}
        if(!size) {
            // Horizon exposes holes as free ranges. SDK startup walks from address zero.
            uint64_t lo=0,hi=1ULL<<39;
            auto boundary=[&](uint64_t begin,uint64_t count) {
                if(begin+count<=address) lo=std::max(lo,begin+count);
                if(begin>address) hi=std::min(hi,begin);
            };
            boundary(c->stack,c->stack_size);
            for(const auto& m:images) boundary(reinterpret_cast<uintptr_t>(m.base),m.size);
            for(const auto& m:mappings) boundary(m.base,m.size);
            if(heap_size) boundary(reinterpret_cast<uintptr_t>(address_space)+heap_offset,heap_size);
            if(address>=hi) fail("QueryMemory exceeds 39-bit address space",uc);
            b=lo;size=hi-lo;
        }
        auto* out=reinterpret_cast<uint64_t*>(r[0]);out[0]=b;out[1]=size;
        auto* p=reinterpret_cast<uint32_t*>(out);p[4]=state;p[5]=attribute;p[6]=permission;p[7]=0;p[8]=0;p[9]=0;
        r[0]=0;r[1]=0;return;
    }
    if(id==0x2c || id==0x2d) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_physical(id==0x2c,r[0],r[1]);return;
    }
    if(id==0x04 || id==0x05) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_stack(id==0x04,r[0],r[1],r[2]);return;
    }
    if(id==0x01) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_resize_heap(r[1]);
        if(!r[0])r[1]=reinterpret_cast<uintptr_t>(address_space)+heap_offset;return;
    }
    if(id==0x26) { log("Guest Break reason=%llx detail=%llx size=%llx",r[0],r[1],r[2]);fail("Guest SDK abort",uc); }
    log("Unimplemented SVC 0x%x",id); fail("Required system call unavailable",uc);
}
}
extern "C" void ot2_host_log(const char* format, ...) {
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);log("%s",text);
}
extern "C" void ot2_host_die(const char* format, ...) {
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);fail(text);
}
extern "C" void guest_thread_init() { create_thread_context(); }
#ifdef OT2_EXECUTION_TESTING
extern "C" int ot2_test_memory_permission(uintptr_t address){std::lock_guard lock(mapping_mutex);return memory_permission_at(address);}
extern "C" uint64_t ot2_test_memory_permission_scans(){std::lock_guard lock(mapping_mutex);return memory_permission_scans;}
extern "C" bool ot2_test_memory_init(uint64_t* regions){
    if(address_space)return false;
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(address_space==MAP_FAILED){address_space=nullptr;return false;}
    regions[0]=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    regions[1]=reinterpret_cast<uintptr_t>(address_space)+stack_offset;
    regions[2]=reinterpret_cast<uintptr_t>(address_space)+alias_offset;
    regions[3]=alias_size;regions[4]=heap_capacity;regions[5]=stack_capacity;
    svc_thread_main_init();thread_system_ready=true;
    return true;
}
extern "C" bool ot2_test_memory_page_size(size_t page){
    if(page<static_cast<size_t>(sysconf(_SC_PAGESIZE))||page<4096||(page&(page-1)))return false;
    memory_host_page=page;return true;
}
extern "C" uintptr_t ot2_test_memory_module(){
    auto* raw=static_cast<uint8_t*>(mmap(nullptr,5*memory_host_page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(raw==MAP_FAILED)return 0;
    const auto base=(reinterpret_cast<uintptr_t>(raw)+memory_host_page-1)&~uintptr_t(memory_host_page-1);
    if(mprotect(reinterpret_cast<void*>(base),memory_host_page,PROT_READ|PROT_EXEC)||mprotect(reinterpret_cast<void*>(base+memory_host_page),memory_host_page,PROT_READ))return 0;
    memory_registry_changed();images.push_back({"memory-contract-module",reinterpret_cast<uint8_t*>(base),4*memory_host_page,0,memory_host_page,memory_host_page,2*memory_host_page});
    return base;
}
extern "C" void ot2_test_memory_svc(unsigned id,uint64_t* registers){
    ucontext_t context{};GuestThread thread{};
    memcpy(context.uc_mcontext.regs,registers,31*sizeof(uint64_t));
    svc(id,&context,&thread);
    memcpy(registers,context.uc_mcontext.regs,31*sizeof(uint64_t));
}
extern "C" bool ot2_test_context_fresh(){
    auto* c=create_thread_context();
    uintptr_t callback=0,reused=0;size_t size=0,reused_size=0;
    ot2_guest_context_callback_stack(c,callback,size);
    ot2_guest_context_callback_stack(c,reused,reused_size);
    if(!callback||callback!=reused||size!=1024*1024||size!=reused_size)return false;
    *reinterpret_cast<volatile uint8_t*>(callback)=1;
    *reinterpret_cast<volatile uint8_t*>(callback+size-1)=2;
    bool fresh=c->guest_tls!=0x12340000&&c->guest_tlr!=0x56780000;
    c->guest_tls=0x12340000;c->guest_tlr=0x56780000;
    return fresh;
}
extern "C" size_t ot2_test_context_live(){return ot2_guest_context_live();}
extern "C" size_t ot2_test_context_bytes(){return ot2_guest_context_bytes();}
extern "C" bool ot2_test_context_exit(){
    auto* main=create_thread_context();install_handlers();main->handle=svc_thread_main_init();thread_system_ready=true;
    auto* code=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED)return false;
    const uint32_t instructions[]={0x94000008,0xd4000141,0xd65f03c0},host_break=0xd420e060;
    memcpy(code,instructions,sizeof(instructions));memcpy(code+32,&host_break,4);memcpy(code+64,instructions+1,8);patch(code,4096);
    traps.push_back({reinterpret_cast<uintptr_t>(code+32),host_break,"context_exit_fixture",[](uint64_t* r)->uint64_t{
        auto* c=current();c->guest_tls=0x12340000;c->guest_tlr=0x56780000;
        if(r[0]){const uint64_t args[8]{};ot2_call_guest_address(r[0],args);}
        return 0;
    }});
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+4096));
    if(mprotect(code,4096,PROT_READ|PROT_EXEC))return false;
    const auto baseline=ot2_guest_context_live(),bytes=ot2_guest_context_bytes();
    for(unsigned i=0;i<400;i++){
        const size_t size=128*1024;auto* stack=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(stack==MAP_FAILED)return false;
        {std::lock_guard lock(mapping_mutex);memory_registry_changed();mappings.push_back({reinterpret_cast<uintptr_t>(stack),size,5,0});}
        uint64_t r[32]{};r[1]=reinterpret_cast<uintptr_t>(code);r[2]=(i&1)?reinterpret_cast<uintptr_t>(code+64):0;
        r[3]=reinterpret_cast<uintptr_t>(stack)+size;r[4]=44;svc_thread(0x08,r);if(r[0])return false;
        uint32_t handle=r[1];r[0]=handle;svc_thread(0x09,r);if(r[0])return false;
        r[1]=reinterpret_cast<uintptr_t>(&handle);r[2]=1;r[3]=2000000000;svc_thread(0x18,r);if(r[0])return false;
        // Completion must already be on the host stack; SDK is free to release this now.
        {std::lock_guard lock(mapping_mutex);memory_registry_changed();std::erase_if(mappings,[&](const Mapping& m){return m.base==reinterpret_cast<uintptr_t>(stack);});}
        munmap(stack,size);r[0]=handle;svc_thread(0x16,r);if(r[0])return false;
        for(unsigned retry=0;retry<10000 && ot2_guest_context_live()!=baseline;retry++)usleep(100);
        if(ot2_guest_context_live()!=baseline||ot2_guest_context_bytes()!=bytes)return false;
    }
    std::erase_if(traps,[&](const Trap& t){return t.pc>=reinterpret_cast<uintptr_t>(code)&&t.pc<reinterpret_cast<uintptr_t>(code+4096);});munmap(code,4096);return true;
}
#endif
extern "C" void crash_thread_init() {}
uintptr_t callback_stack(GuestThread* c){uintptr_t sp;asm volatile("mov %0,sp":"=r"(sp));
    if(sp>=c->stack&&sp<c->stack+c->stack_size)return (sp-1024)&~uintptr_t(15);
    if(service_callback_stack)return service_callback_stack;
    return active_signal?(active_signal->uc_mcontext.sp-256)&~uintptr_t(15):0;
}
extern "C" uint64_t ot2_call_on_host_stack(uint64_t (*function)(uint64_t*),uint64_t* args){
    auto* c=current();if(!c||!c->host_frame)fail("Host service outside guest call");
    struct Invocation {uint64_t (*function)(uint64_t*);uint64_t* args;uintptr_t callback;};
    Invocation invocation{function,args,callback_stack(c)};
    auto invoke=+[](uint64_t* raw)->uint64_t{
        auto& call=*reinterpret_cast<Invocation*>(raw);auto* c=current();
        const auto previous=service_callback_stack,base=c->stack,size=c->stack_size;
        if(host_service_depth==0){
            ot2_guest_context_callback_stack(c,c->stack,c->stack_size);
            service_callback_stack=c->stack+c->stack_size;
        }else service_callback_stack=call.callback;
        ++host_service_depth;auto result=call.function(call.args);--host_service_depth;
        c->stack=base;c->stack_size=size;service_callback_stack=previous;return result;
    };
    // host_frame is the suspended ot2_guest_call frame. A nested callback
    // establishes a new frame below the current native service automatically.
    return ot2_host_stack_call(invoke,reinterpret_cast<uint64_t*>(&invocation),(c->host_frame-1024)&~uintptr_t(15));
}
extern "C" uint64_t ot2_call_guest(const char* name,const uint64_t* args) {
    auto* c=current();if(!c || !active_game) fail("Guest callback outside active game");
    auto entry=active_game->symbol(name);if(!entry) fail("Guest callback export absent");
    uintptr_t top=callback_stack(c);
    if(top && top<c->stack+4096) fail("Nested guest callback stack exhausted");
    return ot2_guest_call(entry,c,args,top);
}
extern "C" uint64_t ot2_guest_symbol_address(const char* name) {
    if(!active_game)fail("Guest symbol outside active game");
    return active_game->symbol(name);
}
extern "C" uint64_t ot2_call_guest_address(uint64_t address,const uint64_t* args) {
    auto* c=current();if(!c || !address)fail("Invalid guest callback");
    uintptr_t top=callback_stack(c);
    if(top && top<c->stack+4096)fail("Nested guest callback stack exhausted");
    return ot2_guest_call(address,c,args,top);
}
extern "C" void* ot2_native_gate(const char* name,void* function) {
    std::lock_guard lock(native_gate_mutex);
    unsigned count=native_gate_count.load();
    for(unsigned i=0;i<count;i++)if(!strcmp(native_gates[i].name,name))return native_gate_code+i*32;
    if(count>=2048 || strlen(name)>=sizeof(native_gates[0].name))fail("Native gate capacity exceeded");
    if(!native_gate_code) {
        native_gate_code=static_cast<uint8_t*>(mmap(nullptr,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(native_gate_code==MAP_FAILED)fail("Native gate allocation failed");
        for(unsigned i=0;i<2048;i++){
            auto* stub=native_gate_code+i*32;uint32_t code[]={0x52800010U|(i<<5),0x58000071,0xd61f0220,0xd503201f};
            memcpy(stub,code,sizeof(code));auto target=reinterpret_cast<uintptr_t>(ot2_native_entry);memcpy(stub+16,&target,8);
        }
        __builtin___clear_cache(reinterpret_cast<char*>(native_gate_code),reinterpret_cast<char*>(native_gate_code+65536));
        if(mprotect(native_gate_code,65536,PROT_READ|PROT_EXEC))fail("Native gate protection failed");
    }
    strcpy(native_gates[count].name,name);native_gates[count].function=function;native_gates[count].present=!strcmp(name,"nvnQueuePresentTexture");
    __atomic_store_n(&ot2_native_abis[count],native_register_abi(name),__ATOMIC_RELEASE);
    native_gate_count.store(count+1,std::memory_order_release);
    return native_gate_code+count*32;
}
extern "C" void* ot2_native_function(unsigned index){
    if(index>=native_gate_count.load(std::memory_order_acquire))fail("Native gate index invalid");
    auto& gate=native_gates[index];if(__atomic_fetch_add(&gate.hits,1,__ATOMIC_RELAXED)==0)log("NATIVE %s",gate.name);
    if(gate.present)update_frame_rate();
    if(!gate.function){log("Missing native API: %s",gate.name);fail("Required native API unavailable");}return gate.function;
}
extern "C" void dm_thread_enter(uint64_t entry,uint64_t argument,uint64_t stack_top) {
    auto* c=current();
    if(!c) fail("Worker thread has no execution context");
    bool found=false;
    {
        std::lock_guard lock(mapping_mutex);
        for(const auto& m:mappings) if(stack_top>m.base && stack_top<=m.base+m.size) {
            ot2_guest_context_use_stack(c,m.base,stack_top-m.base);found=true;break;
        }
    }
    if(!found) fail("Worker stack has not been mapped");
    const uint64_t args[]={argument,0,0,0,0,0,0,0};
    ot2_guest_call(entry,c,args,0);
    fail("Worker returned without ExitThread");
}
extern "C" void ot2_signal_handler(int sig,siginfo_t* info,void* raw) {
    auto* uc=static_cast<ucontext_t*>(raw);
    if(sig==SIGSEGV && (info->si_code==SEGV_ACCERR||info->si_code==SEGV_MAPERR)){
        // Linux exposes the ARM64 data-abort ESR in the signal context. Only
        // write faults may be handled by dirty tracking; never swallow execute
        // faults or unrelated guest memory errors.
        auto* records=uc->uc_mcontext.__reserved;size_t offset=0;
        while(offset+sizeof(_aarch64_ctx)<=sizeof(uc->uc_mcontext.__reserved)){
            auto* record=reinterpret_cast<const _aarch64_ctx*>(records+offset);
            if(record->size<sizeof(*record)||record->size>sizeof(uc->uc_mcontext.__reserved)-offset)break;
            if(record->magic==ESR_MAGIC&&record->size>=sizeof(esr_context)){
                const auto esr=reinterpret_cast<const esr_context*>(record)->esr;
                int saved_errno=errno;
                if(dirty_write_fault_candidate(sig,info->si_code,esr)&&nvn_dirty_fault(info->si_addr)){errno=saved_errno;return;}
                break;
            }
            offset+=record->size;
        }
    }
    auto* c=current();
    if(sig!=SIGTRAP) {
        log("Signal %d code=%d address=%p tracked=%d",sig,info->si_code,info->si_addr,nvn_dirty_tracked(info->si_addr,1));
        size_t offset=0;while(offset+sizeof(_aarch64_ctx)<=sizeof(uc->uc_mcontext.__reserved)){
            auto* record=reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved+offset);
            if(record->size<sizeof(*record)||record->size>sizeof(uc->uc_mcontext.__reserved)-offset)break;
            if(record->magic==ESR_MAGIC&&record->size>=sizeof(esr_context))log("FAULT_ESR value=%llx",(unsigned long long)reinterpret_cast<const esr_context*>(record)->esr);
            offset+=record->size;
        }
        fail(c?"Guest fault":"Signal outside guest thread",uc);
    }
    if(!c) fail("Signal outside guest thread",uc);
    Trap* found=nullptr;
    static std::array<std::atomic<uint32_t>,32768> trap_lookup{};
    auto& cached=trap_lookup[(uc->uc_mcontext.pc>>2)&(trap_lookup.size()-1)];const auto index=cached.load(std::memory_order_relaxed);
    if(index&&index<=traps.size()&&traps[index-1].pc==uc->uc_mcontext.pc)found=&traps[index-1];
    else for(size_t i=0;i<traps.size();++i)if(traps[i].pc==uc->uc_mcontext.pc){found=&traps[i];cached.store(i+1,std::memory_order_relaxed);break;}
    if(!found) fail("Unregistered breakpoint",uc);
    if(found->hook) {
        sdk_signal_count.fetch_add(1,std::memory_order_relaxed);
        if(__atomic_fetch_add(&found->hits,1,__ATOMIC_RELAXED)==0) log("HOST %s",found->hook);
        if(found->call) {
            auto* previous=active_signal;active_signal=uc;
            uc->uc_mcontext.regs[0]=found->call(reinterpret_cast<uint64_t*>(uc->uc_mcontext.regs));
            active_signal=previous;
        }
        uc->uc_mcontext.pc=uc->uc_mcontext.regs[30];
        return;
    }
    auto op=found->instruction;auto target=op&31;auto kind=op&~31U;
    if(kind==0xd53bd040||kind==0xd53bd060||kind==0xd51bd040)tls_signal_count.fetch_add(1,std::memory_order_relaxed);
    auto* r=uc->uc_mcontext.regs;
    if((op&0xffe0001f)==0xd4000001) {svc_signal_count.fetch_add(1,std::memory_order_relaxed);svc((op>>5)&0xffff,uc,c);}
    else if(kind==0xd51bd040) c->guest_tls=target==31?0:r[target];
    else if(target!=31) {
        switch(kind) {
        case 0xd53bd040:r[target]=c->guest_tls;break;
        case 0xd53bd060:r[target]=c->guest_tlr;break;
        case 0xd53be020:counter_signal_count.fetch_add(1,std::memory_order_relaxed);r[target]=ticks();break;
        case 0xd53be000:counter_signal_count.fetch_add(1,std::memory_order_relaxed);r[target]=19200000;break;
        default:fail("Unknown patched instruction",uc);
        }
    }
    uc->uc_mcontext.pc+=4;
}
namespace ot2 {
void freeze_guest_clock(unsigned reason,bool on){guest_clock.freeze(reason,on,monotonic_ns);}
void set_frame_rate_target(int fps){requested_frame_rate.store(fps==60?60:30,std::memory_order_relaxed);}
int frame_rate_applied(){return applied_frame_rate.load(std::memory_order_relaxed);}
static double host_abi_fixture(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,double j,double k) {
    return a+b+c+d+e+f+g+h+i+j+k;
}
std::string execution_probe() {
    std::lock_guard lock(setup_mutex);
    auto* c=create_thread_context();install_handlers();
    // Deliberately overwrite guest x18; cross TLS and SVC gates, then return.
    uint32_t instructions[]={0xd280f692,0xd53bd040,0xd40003c1,0xaa1203e0,0xd65f03c0};
    auto* code=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED) throw std::runtime_error("Execution probe allocation failed");
    memcpy(code,instructions,sizeof(instructions));patch(code,sizeof(instructions));
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+sizeof(instructions)));
    if(mprotect(code,4096,PROT_READ|PROT_EXEC)) throw std::runtime_error("Execution probe mprotect failed");
    const uint64_t args[8]{};const auto value=ot2_guest_call(reinterpret_cast<uintptr_t>(code),c,args,0);
    if(value!=1972) throw std::runtime_error("Guest x18 did not survive host trap");
    auto* nested=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(nested==MAP_FAILED) throw std::runtime_error("Nested probe allocation failed");
    const uint32_t outer[]={0xa9bf7bfd,0xd280f692,0x9400000e,0x8b120000,0xa8c17bfd,0xd65f03c0};
    std::vector<uint32_t> inner{0xd11003ff,0xa9007bfd};
    for(unsigned offset=16;offset<1024;offset+=8)inner.push_back(0xf90003ff|((offset/8)<<10));
    for(uint32_t instruction:{0xd280f6b2U,0xd40003c1U,0xaa1203e0U,0xa9407bfdU,0x911003ffU,0xd65f03c0U})inner.push_back(instruction);
    const uint32_t host_break=0xd420e060;
    memcpy(nested,outer,sizeof(outer));memcpy(nested+64,&host_break,4);memcpy(nested+128,inner.data(),inner.size()*4);
    patch(nested+128,inner.size()*4);
    traps.push_back({reinterpret_cast<uintptr_t>(nested+64),host_break,"nested_execution_fixture",[](uint64_t* r)->uint64_t{
        volatile uint64_t sentinels[16];for(unsigned i=0;i<16;++i)sentinels[i]=0x5a5a0000+i;
        const uint64_t arguments[8]{};auto* thread=current();
        const auto top=(active_signal->uc_mcontext.sp-256)&~uintptr_t(15);
        auto result=ot2_guest_call(r[0],thread,arguments,top);
        for(unsigned i=0;i<16;++i)if(sentinels[i]!=0x5a5a0000+i)fail("Nested signal overwrote host frame");
        return result;
    }});
    __builtin___clear_cache(reinterpret_cast<char*>(nested),reinterpret_cast<char*>(nested+4096));
    if(mprotect(nested,4096,PROT_READ|PROT_EXEC)) throw std::runtime_error("Nested probe mprotect failed");
    const uint64_t nested_args[]={reinterpret_cast<uintptr_t>(nested+128),0,0,0,0,0,0,0};
    const auto nested_value=ot2_guest_call(reinterpret_cast<uintptr_t>(nested),c,nested_args,0);
    if(nested_value!=3945) throw std::runtime_error("Nested guest register restoration failed");
    auto* direct=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(direct==MAP_FAILED)throw std::runtime_error("Direct callback probe allocation failed");
    memcpy(direct,outer,sizeof(outer));const uint32_t jump[]={0x58000050,0xd61f0200};memcpy(direct+64,jump,sizeof(jump));
    auto callback=+[](uint64_t entry)->uint64_t{volatile uint64_t guard[16];for(unsigned i=0;i<16;i++)guard[i]=0x12340000+i;const uint64_t arguments[8]{};auto value=ot2_call_guest_address(entry,arguments);for(unsigned i=0;i<16;i++)if(guard[i]!=0x12340000+i)fail("Direct callback overwrote native stack");return value;};
    auto gate=reinterpret_cast<uintptr_t>(ot2_native_gate("direct_nested_fixture",reinterpret_cast<void*>(callback)));memcpy(direct+72,&gate,8);
    __builtin___clear_cache(reinterpret_cast<char*>(direct),reinterpret_cast<char*>(direct+4096));if(mprotect(direct,4096,PROT_READ|PROT_EXEC))throw std::runtime_error("Direct callback probe protection failed");
    const auto direct_value=ot2_guest_call(reinterpret_cast<uintptr_t>(direct),c,nested_args,0);if(direct_value!=3945)throw std::runtime_error("Direct native callback register restoration failed");
    const uint64_t abi_args[]={reinterpret_cast<uint64_t>(ot2_native_gate("abi_fixture",reinterpret_cast<void*>(host_abi_fixture))),0,0,0,0,0,0,0};
    auto abi_value=ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_abi_fixture),c,abi_args,0);
    if(abi_value!=49)throw std::runtime_error("Native floating-point/stack ABI failed");
    struct {alignas(16) uint8_t view[16];uint64_t guards[2];} view_fixture{};
    view_fixture.guards[0]=0x12345678abcd0001;view_fixture.guards[1]=0x12345678abcd0002;
    const uint64_t view_args[]={reinterpret_cast<uint64_t>(view_fixture.view),3,2,0,0,0,0,0};
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureViewSetDefaults")),c,view_args,0);
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureViewSetLevels")),c,view_args,0);
    if(view_fixture.guards[0]!=0x12345678abcd0001 || view_fixture.guards[1]!=0x12345678abcd0002)throw std::runtime_error("NVN view exceeds game ABI size");
    struct {alignas(16) uint8_t texture[128];tex_builder_t builder;} texture_fixture{};
    texture_fixture.builder.d.target=NVN_TEXTURE_TARGET_BUFFER;texture_fixture.builder.d.format=NVN_FORMAT_R32UI;
    texture_fixture.builder.d.width=32;texture_fixture.builder.d.height=texture_fixture.builder.d.depth=texture_fixture.builder.d.levels=1;
    const auto saved_builder=texture_fixture.builder;
    const uint64_t texture_args[]={reinterpret_cast<uint64_t>(texture_fixture.texture),reinterpret_cast<uint64_t>(&texture_fixture.builder),0,0,0,0,0,0};
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureInitialize")),c,texture_args,0);
    if(memcmp(&saved_builder,&texture_fixture.builder,sizeof(saved_builder)))throw std::runtime_error("NVN texture writes beyond 128-byte game object");
    alignas(16) std::array<uint8_t,256> source_memory{},copy_memory{};cmd_t source_cmd{},copy_cmd{};
    auto call_nvn=[&](const char* name,std::initializer_list<uint64_t> values){uint64_t a[8]{};std::copy(values.begin(),values.end(),a);return ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup(name)),c,a,0);};
    for(auto item:{std::pair{&source_cmd,source_memory.data()},std::pair{&copy_cmd,copy_memory.data()}}){
        call_nvn("nvnCommandBufferInitialize",{reinterpret_cast<uint64_t>(item.first),0});
        pool_t pool{MAGIC('P','O','O','L'),0,item.second,256};
        call_nvn("nvnCommandBufferAddCommandMemory",{reinterpret_cast<uint64_t>(item.first),reinterpret_cast<uint64_t>(&pool),0,256});
        call_nvn("nvnCommandBufferBeginRecording",{reinterpret_cast<uint64_t>(item.first)});
    }
    call_nvn("nvnCommandBufferSetCopyRowStride",{reinterpret_cast<uint64_t>(&source_cmd),1234});
    uint64_t handle=call_nvn("nvnCommandBufferEndRecording",{reinterpret_cast<uint64_t>(&source_cmd)});
    call_nvn("nvnCommandBufferCopyCommands",{reinterpret_cast<uint64_t>(&copy_cmd),1,reinterpret_cast<uint64_t>(&handle)});
    call_nvn("nvnCommandBufferEndRecording",{reinterpret_cast<uint64_t>(&copy_cmd)});
    source_memory.fill(0xdd);op_hdr copied{};int64_t stride=0;memcpy(&copied,copy_memory.data()+8,8);memcpy(&stride,copy_memory.data()+16,8);
    if(copied.op!=OP_COPY_STRIDE || stride!=1234)throw std::runtime_error("NVN CopyCommands borrowed reusable source memory");
    alignas(16) std::array<uint8_t,64> keyboard;keyboard.fill(0xa5);uint64_t keyboard_args[]={reinterpret_cast<uint64_t>(keyboard.data()),0,0,0,0,0,0,0};
    bool keyboard_called=false;for(auto& service:input_services())if(!strcmp(service.name,"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE")){service.call(keyboard_args);keyboard_called=true;}
    if(!keyboard_called)throw std::runtime_error("Keyboard state service absent");
    for(size_t i=8;i<keyboard.size();i++)if(keyboard[i]!=(i<48?0:0xa5))throw std::runtime_error("Keyboard state ABI size mismatch");
    return "{\"direct_native_callback\":true,\"guest_x18\":"+std::to_string(value)+",\"tls_and_svc_traps\":true,\"nested_return\":"+std::to_string(nested_value)+",\"native_abi_return\":"+std::to_string(abi_value)+",\"nvn_view_16_bytes\":true,\"nvn_texture_128_bytes\":true,\"nvn_command_copy_owned\":true,\"keyboard_state_48_bytes\":true}";
}
extern "C" void ot2_guest_batch_profile(int enabled){
    ot2::set_io_prefetch(enabled!=0);__atomic_store_n(&fast_system_enabled,enabled!=0,__ATOMIC_RELEASE);
    ot2_host_log("SYSTEM_GATES enabled=%d calls=%llu",enabled,(unsigned long long)system_fast_calls.exchange(0));
}
extern "C" void ot2_guest_register_gates(int enabled){
    __atomic_store_n(&ot2_register_gates_enabled,enabled!=0,__ATOMIC_RELEASE);
}
extern "C" void ot2_guest_execution_profile(int enable_tls){
    log("EXECUTION_BOUNDARIES tls_fast=%u tls_traps=%llu svc_traps=%llu counter_traps=%llu sdk_traps=%llu",
        __atomic_load_n(&fast_tls_enabled,__ATOMIC_ACQUIRE),(unsigned long long)tls_signal_count.exchange(0),
        (unsigned long long)svc_signal_count.exchange(0),(unsigned long long)counter_signal_count.exchange(0),(unsigned long long)sdk_signal_count.exchange(0));
    __atomic_store_n(&fast_tls_enabled,enable_tls!=0,__ATOMIC_RELEASE);
}
extern "C" void ot2_guest_permission_profile(int enabled){
    std::lock_guard lock(mapping_mutex);
    log("MEMORY_PERMISSION enabled=%u scans=%llu hits=%llu mappings=%zu",memory_permission_cache_enabled,(unsigned long long)memory_permission_scans,(unsigned long long)memory_permission_hits,mappings.size());
    memory_permission_cache_enabled=enabled!=0;memory_permission_scans=memory_permission_hits=0;
}
void boot_game(const std::string& directory,const std::string& log_path,uint64_t program_id) {
    log_fd=open(log_path.c_str(),O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0600);
    log("Dedicated native host: ProgramId=%016llx",(unsigned long long)program_id);
    auto user_path=(std::filesystem::path(log_path).parent_path()/"userdata").string();
    std::filesystem::create_directories(user_path);
    vkhost_configure_package((std::filesystem::path(log_path).parent_path()/"shaders").c_str(),(std::filesystem::path(directory).parent_path().parent_path()/"shaders").c_str());
    configure_services((std::filesystem::path(directory).parent_path()/"RomFS").string(),user_path,program_id);
    auto* c=create_thread_context();install_handlers();
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(address_space==MAP_FAILED) fail("Address space reserve failed");
    c->handle=svc_thread_main_init();thread_system_ready=true;
    LoadedGame game(directory);active_game=&game;memory_registry_changed();images=game.modules();
    for(const auto& m:images)if(m.name=="main")frame_rate_patch.configure(reinterpret_cast<uintptr_t>(m.base),m.size,m.build_id);
    for(auto& m:images) { image_size+=m.size;patch(m.base+m.text_offset,m.text_size);log("Module %s base=%p size=%zu",m.name.c_str(),m.base,m.size); }
    install_service_initializers(game);
    game.make_executable();log("Patched %zu system boundaries; running ARM64 directly",traps.size());
    auto call=[&](const char* name,uint64_t arg0=0,uint64_t arg1=0) {
        auto address=game.symbol(name);if(!address) {log("Missing %s",name);fail("Required entry absent");}
        const uint64_t args[]={arg0,arg1,0,0,0,0,0,0};
        log("ENTER %s",name);auto result=ot2_guest_call(address,c,args,0);log("RETURN %s = %llx",name,(unsigned long long)result);
    };
    call("_init_libc0");call("nnosInitialize",c->handle,0);call("_init_libc1");
    call("nninitInitializeSdkModule");call("nninitStartup");call("_init_libc2");
    for(auto address:game.initializers()) {const uint64_t args[8]{};log("DT_INIT %p",reinterpret_cast<void*>(address));ot2_guest_call(address,c,args,0);}
    call("nnMain");
}
#ifdef OT2_EXECUTION_TESTING
#include "tests/guest_sdk_contract.inc"
#include "tests/register_contract.inc"
#include "tests/tls_contract.inc"
#include "tests/native_gate_contract.inc"
#include "tests/host_stack_contract.inc"
#endif
}
