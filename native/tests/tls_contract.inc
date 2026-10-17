// Exercise the same patched instruction repeatedly, including the ordinary
// arithmetic/branch instructions around it. The negative control uses the
// exact same code with fast TLS disabled, not a second implementation.
extern "C" bool ot2_test_tls_contract(){
    std::lock_guard lock(setup_mutex);
    auto* context=create_thread_context();install_handlers();
    size_t page_size=sysconf(_SC_PAGESIZE);auto start=traps.size();
    auto* code=static_cast<uint32_t*>(mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED)throw std::runtime_error("TLS benchmark mapping");
    const uint32_t original[]={0xaa0003e1,0xd53bd060,0xf1000421,0x54ffffc1,0xd65f03c0};
    memcpy(code,original,sizeof original);patch(reinterpret_cast<uint8_t*>(code),sizeof original);
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code)+sizeof original);
    if(mprotect(code,page_size,PROT_READ|PROT_EXEC))throw std::runtime_error("TLS benchmark executable");
    uint64_t elapsed[2]{},counts[2]{};constexpr uint64_t iterations=20000;
    for(unsigned fast=0;fast<2;++fast){
        __atomic_store_n(&fast_tls_enabled,fast,__ATOMIC_RELEASE);tls_signal_count.store(0);
        const uint64_t args[]={iterations,0,0,0,0,0,0,0};auto before=ticks();
        auto result=ot2_guest_call(reinterpret_cast<uintptr_t>(code),context,args,0);
        elapsed[fast]=ticks()-before;counts[fast]=tls_signal_count.load();
        if(result!=context->guest_tlr)throw std::runtime_error("TLS loop returned wrong context");
    }
    __atomic_store_n(&fast_tls_enabled,1,__ATOMIC_RELEASE);traps.resize(start);munmap(code,page_size);
    printf("TLS_LOOP iterations=%llu slow_traps=%llu fast_traps=%llu slow_us=%.3f fast_us=%.3f\n",
        (unsigned long long)iterations,(unsigned long long)counts[0],(unsigned long long)counts[1],elapsed[0]/19.2,elapsed[1]/19.2);
    if(counts[0]!=iterations||counts[1]!=0)throw std::runtime_error("TLS native fast path still enters signal handler");
    if(elapsed[1]>=elapsed[0])throw std::runtime_error("TLS fast path did not reduce isolated cost");
    return true;
}
