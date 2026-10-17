// Actual Android signal return, not a simulation of the handler. Ordinary
// instructions stay unchanged in this host; only the listed system gates are
// patched. Exercise every Rt including XZR with randomized surrounding state.
struct alignas(16) RegisterFixture {
    uint64_t x[31],nzcv,v[64],fpcr,fpsr,sp,pc;
};
static_assert(offsetof(RegisterFixture,v)==256);
static RegisterFixture captured_registers;
static uintptr_t register_epilogue;
extern "C" bool ot2_test_register_contract(){
    std::lock_guard lock(setup_mutex);
    auto* context=create_thread_context();install_handlers();
    auto original_tls=context->guest_tls,original_tlr=context->guest_tlr;
    const auto trap_start=traps.size();size_t pagesize=sysconf(_SC_PAGESIZE);
    auto* page=static_cast<uint32_t*>(mmap(nullptr,pagesize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(page==MAP_FAILED)throw std::runtime_error("register contract mmap");
    uint64_t random=0x2bb26851ae78de61;auto next=[&]{random^=random<<13;random^=random>>7;random^=random<<17;return random;};
    unsigned trials=0,negatives=0;
    for(unsigned fast=0;fast<2;++fast)for(uint32_t kind:{0xd53bd040u,0xd53bd060u,0xd51bd040u,0xd53be020u,0xd53be000u,0xd40003c1u})for(unsigned target=0;target<(kind==0xd40003c1?1u:32u);target++){
        __atomic_store_n(&fast_tls_enabled,fast,__ATOMIC_RELEASE);__atomic_store_n(&fast_system_enabled,fast,__ATOMIC_RELEASE);
        std::vector<uint32_t> code{0xd10043ff,0xf90003fe,0xaa0003fe}; // save LR, x30 = input state
        auto load_x=[&](unsigned rt,unsigned offset){code.push_back(0xf9400000u|((offset/8)<<10)|(30<<5)|rt);};
        load_x(9,768);code.push_back(0xd51b4409);load_x(9,776);code.push_back(0xd51b4429);
        load_x(9,248);code.push_back(0xd51b4209);
        for(unsigned q=0;q<32;q++)code.push_back(0x3dc00000u|(((256+q*16)/16)<<10)|(30<<5)|q);
        for(unsigned x=0;x<31;x++)load_x(x,x*8);
        auto gate=code.size();code.push_back(kind==0xd40003c1?kind:kind|target);
        auto mutation=code.size();code.push_back(0xd503201f); // negative control replaces this NOP
        auto capture=code.size();code.push_back(0xd420e060);
        register_epilogue=reinterpret_cast<uintptr_t>(page+code.size());
        code.insert(code.end(),{0xf94003fe,0x910043ff,0xd65f03c0});
        if(mprotect(page,pagesize,PROT_READ|PROT_WRITE))throw std::runtime_error("register contract writable");
        memcpy(page,code.data(),code.size()*4);traps.resize(trap_start);patch(reinterpret_cast<uint8_t*>(page),code.size()*4);
        if(traps.size()!=trap_start+1||traps.back().instruction!=code[gate])throw std::runtime_error("patch changed an ordinary instruction");
        for(size_t i=0;i<code.size();i++)if(i!=gate&&page[i]!=code[i])throw std::runtime_error("patch changed ordinary bytes");
        traps.push_back({reinterpret_cast<uintptr_t>(page+capture),0,"register_capture",[](uint64_t* r)->uint64_t{
            const auto& m=active_signal->uc_mcontext;memcpy(captured_registers.x,r,sizeof(captured_registers.x));captured_registers.nzcv=m.pstate&0xf0000000;
            captured_registers.sp=m.sp;captured_registers.pc=m.pc;bool found=false;
            for(size_t at=0;at+sizeof(_aarch64_ctx)<=sizeof(m.__reserved);){
                auto* header=reinterpret_cast<const _aarch64_ctx*>(m.__reserved+at);if(header->size<sizeof(*header)||header->size>sizeof(m.__reserved)-at)break;
                if(header->magic==FPSIMD_MAGIC&&header->size>=sizeof(fpsimd_context)){auto* fp=reinterpret_cast<const fpsimd_context*>(header);memcpy(captured_registers.v,fp->vregs,sizeof(captured_registers.v));captured_registers.fpcr=fp->fpcr;captured_registers.fpsr=fp->fpsr;found=true;break;}at+=header->size;
            }
            if(!found)fail("register contract lacks FPSIMD signal context");
            r[30]=register_epilogue;return r[0];
        }});
        for(unsigned seed=0;seed<9;seed++){
            if(mprotect(page,pagesize,PROT_READ|PROT_WRITE))throw std::runtime_error("register mutation writable");
            page[mutation]=seed==8?0xd2800007:0xd503201f; // MOV x7, #0 must be detected
            __builtin___clear_cache(reinterpret_cast<char*>(page),reinterpret_cast<char*>(page+code.size()));
            if(mprotect(page,pagesize,PROT_READ|PROT_EXEC))throw std::runtime_error("register executable");
            RegisterFixture input{};for(auto& x:input.x)x=next();for(auto& v:input.v)v=next();input.nzcv=next()&0xf0000000;input.fpcr=(next()&3)<<22;input.fpsr=next()&0x1f;
            auto expected=input;context->guest_tls=next();context->guest_tlr=next();auto tls=context->guest_tls;
            bool counter=kind==0xd53be020||kind==0xd40003c1;
            if(kind==0xd51bd040)tls=target==31?0:input.x[target];
            else if(target!=31){if(kind==0xd53bd040)expected.x[target]=context->guest_tls;else if(kind==0xd53bd060)expected.x[target]=context->guest_tlr;else if(kind==0xd53be000)expected.x[target]=19200000;}
            const uint64_t args[]={reinterpret_cast<uint64_t>(&input),0,0,0,0,0,0,0};auto before=ticks();ot2_guest_call(reinterpret_cast<uintptr_t>(page),context,args,0);auto after=ticks();
            bool valid=true;if(counter&&target!=31){valid&=captured_registers.x[target]>=before&&captured_registers.x[target]<=after;expected.x[target]=captured_registers.x[target];}
            valid&=!memcmp(expected.x,captured_registers.x,sizeof(expected.x))&&!memcmp(expected.v,captured_registers.v,sizeof(expected.v));
            valid&=expected.nzcv==captured_registers.nzcv&&expected.fpcr==captured_registers.fpcr&&expected.fpsr==captured_registers.fpsr;
            valid&=captured_registers.sp==context->stack+context->stack_size-16&&captured_registers.pc==reinterpret_cast<uintptr_t>(page+capture)&&context->guest_tls==tls;
            if(seed==8){if(valid)throw std::runtime_error("register mutation was not detected");++negatives;}else{if(!valid){printf("REGISTER_MISMATCH kind=%x target=%u seed=%u\n",kind,target,seed);throw std::runtime_error("system gate corrupted register state");}++trials;}
        }
    }
    __atomic_store_n(&fast_tls_enabled,1,__ATOMIC_RELEASE);__atomic_store_n(&fast_system_enabled,1,__ATOMIC_RELEASE);
    context->guest_tls=original_tls;context->guest_tlr=original_tlr;traps.resize(trap_start);munmap(page,pagesize);
    printf("PASS ARM64 gates: %u randomized native trials, %u mutations rejected, all Rt/XZR, x0-x30, v0-v31, NZCV, FPCR/FPSR, SP/PC\n",trials,negatives);return true;
}
