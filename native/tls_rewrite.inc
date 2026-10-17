// Local branch islands for TLS system-register instructions. Ordinary guest
// instructions and x18 are untouched. No C call, signal, flag-setting op, or
// vector instruction is used on the fast path. The four scratch registers
// exclude Rt, so all 32 register encodings (including XZR) are preserved.
// Islands are populated RW, then sealed RX before any guest thread runs.
struct TlsIsland {uint8_t* memory;size_t bytes,used;};
std::vector<TlsIsland> tls_islands;
bool tls_branch_reachable(uintptr_t from,uintptr_t to){
    int64_t offset=int64_t(to)-int64_t(from);
    return !(offset&3)&&offset>=-(1LL<<27)&&offset<(1LL<<27);
}
uint32_t tls_branch(uintptr_t from,uintptr_t to){
    if(!tls_branch_reachable(from,to))fail("TLS island branch out of range");
    return 0x14000000u|(uint32_t((int64_t(to)-int64_t(from))/4)&0x3ffffffu);
}
uint8_t* tls_allocate_near(uintptr_t pc,size_t bytes){
    // Never replace an existing guest/host mapping. Older kernels may ignore
    // MAP_FIXED_NOREPLACE; verify the returned address before using it.
    constexpr int no_replace=0x100000;
    const uintptr_t anchor=pc&~uintptr_t(bytes-1);
    for(uintptr_t distance=65536;distance<(1u<<27)-65536;distance+=65536){
        for(int sign:{-1,1}){
            auto wanted=uintptr_t(int64_t(anchor)+sign*int64_t(distance));
            auto* p=mmap(reinterpret_cast<void*>(wanted),bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|no_replace,-1,0);
            if(p==MAP_FAILED)continue;
            if(uintptr_t(p)==wanted)return static_cast<uint8_t*>(p);
            munmap(p,bytes);
        }
    }
    return nullptr;
}
bool patch_tls(uintptr_t pc,uint32_t op,size_t first_island){
    auto kind=op&~31u;if(kind!=0xd53bd040&&kind!=0xd53bd060&&kind!=0xd51bd040)return false;
    constexpr size_t capacity=192;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& candidate=tls_islands[i];
        if(candidate.bytes-candidate.used>=capacity&&tls_branch_reachable(pc,uintptr_t(candidate.memory)+candidate.used)&&tls_branch_reachable(uintptr_t(candidate.memory)+candidate.used+capacity,pc+4)){island=&candidate;break;}}
    if(!island){
        size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);
        if(!memory)return false; // Correct signal implementation remains the fallback.
        tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();
    }
    auto* base=island->memory+island->used;auto rt=op&31u;
    uint32_t scratch[4];unsigned n=0;for(unsigned r=9;n<4;++r)if(r!=rt)scratch[n++]=r;
    auto a=scratch[0],b=scratch[1],c=scratch[2],d=scratch[3];
    std::vector<uint32_t> code;auto emit=[&](uint32_t instruction){code.push_back(instruction);return code.size()-1;};
    auto relative=[&](size_t at,size_t to){code[at]|=(uint32_t(int32_t(to)-int32_t(at))&0x7ffffu)<<5;};
    auto restore=[&]{emit(0xa9400000u|(2u<<15)|(d<<10)|(31u<<5)|c);emit(0xa8c00000u|(4u<<15)|(b<<10)|(31u<<5)|a);};
    auto resume=[&]{auto at=uintptr_t(base)+code.size()*4;emit(tls_branch(at,pc+4));};
    emit(0xa9800000u|(124u<<15)|(b<<10)|(31u<<5)|a); // stp a,b,[sp,#-32]!
    emit(0xa9000000u|(2u<<15)|(d<<10)|(31u<<5)|c);   // stp c,d,[sp,#16]
    auto flag_literal=emit(0x58000000u|b);
    emit(0x88dffc00u|(b<<5)|c);                     // ldar Wc,[b]
    auto disabled=emit(0x34000000u|c);              // cbz Wc,slow
    emit(0xd53bd040u|a);                           // actual Android TP
    auto table_literal=emit(0x58000000u|b);
    emit(0xd2800000u|(GuestThreadCapacity<<5)|d);
    auto loop=code.size();emit(0xc8dffc00u|(b<<5)|c);// acquire published context
    emit(0xca000000u|(a<<16)|(c<<5)|c);             // eor; NZCV unchanged
    auto found=emit(0xb4000000u|c);
    emit(0x91000000u|(sizeof(GuestThread)<<10)|(b<<5)|b);
    emit(0xd1000000u|(1u<<10)|(d<<5)|d);
    auto again=emit(0xb5000000u|d);relative(again,loop);
    auto absent=emit(0x14000000u);
    relative(found,code.size());
    if(kind==0xd51bd040)emit(0xf9000000u|((offsetof(GuestThread,guest_tls)/8)<<10)|(b<<5)|rt);
    else if(rt!=31){auto offset=kind==0xd53bd040?offsetof(GuestThread,guest_tls):offsetof(GuestThread,guest_tlr);emit(0xf9400000u|((offset/8)<<10)|(b<<5)|rt);}
    restore();resume();
    auto slow=code.size();relative(disabled,slow);code[absent]|=uint32_t(slow-absent)&0x3ffffffu;
    restore();auto trap=emit(0xd4200000u|(0x702u<<5));resume();
    if(code.size()&1)emit(0xd503201fu);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    relative(flag_literal,literal(uintptr_t(&fast_tls_enabled)));relative(table_literal,literal(uintptr_t(ot2_threads)));
    if(code.size()*4>capacity)fail("TLS island capacity exceeded");
    memcpy(base,code.data(),code.size()*4);island->used+=(code.size()*4+15)&~size_t(15);
    traps.push_back({uintptr_t(base)+trap*4,op,nullptr});
    const auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);return true;
}
void seal_tls_islands(size_t first){
    for(size_t i=first;i<tls_islands.size();++i){auto& block=tls_islands[i];
        __builtin___clear_cache(reinterpret_cast<char*>(block.memory),reinterpret_cast<char*>(block.memory)+block.used);
        if(mprotect(block.memory,block.bytes,PROT_READ|PROT_EXEC))fail("TLS island executable protection");}
}
