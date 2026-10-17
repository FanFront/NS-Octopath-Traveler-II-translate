// A movie decoder can be called from a 16 KiB SDK worker. Android libraries
// need a native stack; callbacks must resume below the suspended guest frame.
extern "C" bool ot2_test_host_stack(){
    std::lock_guard lock(setup_mutex);auto* c=create_thread_context();install_handlers();
    const size_t page=sysconf(_SC_PAGESIZE),size=16384;
    auto* region=static_cast<uint8_t*>(mmap(nullptr,size+page*2,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    auto* code=static_cast<uint8_t*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(region==MAP_FAILED||code==MAP_FAILED)fail("Host stack fixture allocation");
    if(mprotect(region+page,size,PROT_READ|PROT_WRITE))fail("Host stack fixture protection");
    const uint32_t outer[]={0xa9bf7bfd,0xd280f692,0x9400000e,0x8b120000,0xa8c17bfd,0xd65f03c0};
    const uint32_t inner[]={0xa9bf7bfd,0xd2800540,0x9400001e,0xa8c17bfd,0xd65f03c0};
    memcpy(code,outer,sizeof(outer));memcpy(code+128,inner,sizeof(inner));
    auto second=+[](uint64_t* r)->uint64_t{return ot2_call_on_host_stack(+[](uint64_t* a)->uint64_t{
        uintptr_t sp;asm volatile("mov %0,sp":"=r"(sp));auto* t=current();
        if(sp>=t->stack&&sp<t->stack+t->stack_size)fail("Nested service remained on guest stack");
        return a[0]+1;
    },r);};
    auto first=+[](uint64_t* r)->uint64_t{return ot2_call_on_host_stack(+[](uint64_t* a)->uint64_t{
        volatile uint8_t scratch[65536];for(size_t i=0;i<sizeof(scratch);i+=4096)scratch[i]=uint8_t(i/4096+1);
        const uint64_t arguments[8]{};auto result=ot2_call_guest_address(a[0],arguments);
        for(size_t i=0;i<sizeof(scratch);i+=4096)if(scratch[i]!=uint8_t(i/4096+1))fail("Callback overwrote native stack");
        return result;
    },r);};
    const auto first_island=tls_islands.size();
    if(!patch_system(uintptr_t(code+64),0xd65f03c0,first_island,"host_stack_fixture",first)||
       !patch_system(uintptr_t(code+256),0xd65f03c0,first_island,"host_stack_nested_fixture",second))fail("Host stack fixture gates");
    seal_tls_islands(first_island);__builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+page));
    if(mprotect(code,page,PROT_READ|PROT_EXEC))fail("Host stack fixture executable");
    auto saved_base=c->stack,saved_size=c->stack_size;auto saved_fast=fast_system_enabled;
    c->stack=uintptr_t(region+page);c->stack_size=size;
    const uint64_t args[]={uintptr_t(code+128),0,0,0,0,0,0,0};
    for(unsigned fast:{0u,1u}){
        fast_system_enabled=fast;
        for(unsigned i=0;i<32;i++)if(ot2_guest_call(uintptr_t(code),c,args,0)!=2015)fail("Host/guest stack return or x18 mismatch");
    }
    c->stack=saved_base;c->stack_size=saved_size;fast_system_enabled=saved_fast;
    munmap(region,size+page*2);munmap(code,page);
    puts("PASS 16 KiB guarded guest stack, 64 KiB native frame, nested guest/host callbacks, x18, slow and fast gates");return true;
}
