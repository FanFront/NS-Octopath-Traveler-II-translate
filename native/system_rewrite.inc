// Full-state host gates for elapsed-time reads and SDK service entry points.
// The tiny island never overwrites a second guest instruction. x16/x17/LR are
// saved before entering host code; every other register and FP state is saved
// by ot2_system_entry. Slow BRK sites remain available for negative controls.
alignas(4) uint32_t fast_system_enabled=1;
struct SystemService {const char* name;uint64_t (*call)(uint64_t*);};
std::vector<SystemService> system_services;
std::atomic<uint64_t> system_fast_calls{0};
extern "C" void ot2_system_entry();
void svc(unsigned,ucontext_t*,GuestThread*);
extern "C" void ot2_system_dispatch(uint64_t selector,uint64_t* registers){
    // Assembly stores its own return link separately at word 31.
    auto* saved=registers+100;
    registers[16]=saved[0];registers[17]=saved[1];registers[30]=saved[2];
    if(selector&0x8000){const auto& service=system_services.at(selector&0x7fff);if(service.call)registers[0]=service.call(registers);}
    else if(selector&0x4000){
        ucontext_t context{};memcpy(context.uc_mcontext.regs,registers,31*8);
        context.uc_mcontext.sp=reinterpret_cast<uintptr_t>(saved+4);context.uc_mcontext.pc=registers[31];
        auto* previous=active_signal;active_signal=&context;svc(selector&0x3fff,&context,current());active_signal=previous;
        memcpy(registers,context.uc_mcontext.regs,31*8);
    }
    else {const auto target=selector&31;if(target!=31)registers[target]=(selector&32)?19200000:ticks();}
    saved[0]=registers[16];saved[1]=registers[17];saved[2]=registers[30];
    system_fast_calls.fetch_add(1,std::memory_order_relaxed);
}
bool patch_system(uintptr_t pc,uint32_t op,size_t first_island,const char* name=nullptr,uint64_t (*call)(uint64_t*)=nullptr){
    const auto kind=op&~31u;const bool service=name!=nullptr;const bool syscall=(op&0xffe0001f)==0xd4000001;
    if(!service&&kind!=0xd53be020&&kind!=0xd53be000&&!syscall)return false;
    if(syscall&&((op>>5)&0xffff)>=0x4000)return false;
    if(service&&system_services.size()>=0x8000)return false;
    constexpr size_t capacity=112;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& c=tls_islands[i];if(c.bytes-c.used>=capacity&&tls_branch_reachable(pc,uintptr_t(c.memory)+c.used)&&tls_branch_reachable(uintptr_t(c.memory)+c.used+capacity,pc+4)){island=&c;break;}}
    if(!island){const size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);if(!memory)return false;tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();}
    auto* base=island->memory+island->used;
    const uint32_t selector=service?0x8000|uint32_t(system_services.size()):op==0xd40003c1?0u:syscall?0x4000|((op>>5)&0x3fff):(op&31)|(kind==0xd53be000?32u:0u);
    std::vector<uint32_t> code;auto emit=[&](uint32_t word){code.push_back(word);return code.size()-1;};
    auto relative=[&](size_t at,size_t to){code[at]|=(uint32_t(int32_t(to)-int32_t(at))&0x7ffffu)<<5;};
    auto restore=[&]{emit(0xf9400bfe);emit(0xa8c247f0);}; // ldr LR,[sp,16]; ldp x16,x17,[sp],32
    auto resume=[&]{if(service)emit(0xd65f03c0);else emit(tls_branch(uintptr_t(base)+code.size()*4,pc+4));};
    emit(0xa9be47f0);emit(0xf9000bfe); // stp x16,x17,[sp,-32]!; str LR,[sp,16]
    auto flag=emit(0x58000010);emit(0x88dffe11); // ldr x16,literal; ldar w17,[x16]
    auto disabled=emit(0x34000011);
    emit(0x52800010|(selector<<5));auto entry=emit(0x58000011);emit(0xd63f0220);
    restore();resume();relative(disabled,code.size());restore();
    auto trap=emit(0xd4200000|((service?0x703:0x702)<<5));resume();
    if(code.size()&1)emit(0xd503201f);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    relative(flag,literal(uintptr_t(&fast_system_enabled)));relative(entry,literal(uintptr_t(ot2_system_entry)));
    if(code.size()*4>capacity)fail("System gate island capacity");
    if(service)system_services.push_back({name,call});
    traps.push_back({uintptr_t(base)+trap*4,op,name,call});
    memcpy(base,code.data(),code.size()*4);island->used+=(code.size()*4+15)&~size_t(15);
    auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);return true;
}
