// Compare the real storage-descriptor decoder against the original padded
// bank, including partial words, bank ends, same-address mutations and all
// supported synthetic tables. The old expansion must trip the cost assertion.
extern "C" int vkhost_test_constant_decode(){
    drain_queues();std::lock_guard lock(state_mutex);
    const auto saved=render_state;Shader shader;
    std::array<uint8_t,65536> bytes{};
    uint32_t random=0x1972a130;auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(auto& b:bytes)b=next();shader.c1.assign(bytes.begin(),bytes.begin()+27);
    for(int stage=0;stage<6;stage++)for(int slot=0;slot<16;slot++){
        auto& storage=render_state.storage[stage][slot];storage.addr=uint64_t(next())<<32|next();storage.size=next();
        auto& uniform=render_state.uniforms[stage][slot];uniform.addr=uintptr_t(bytes.data());uniform.size=slot==0?13:slot==1?65535:31+slot;
    }
    for(int stage=0;stage<6;stage++)for(int slot=0;slot<128;slot++){
        render_state.texture_handles[stage][slot]=next();render_state.sampler_handles[stage][slot]=next();render_state.image_handles[stage][slot]=next();
    }
    uint64_t expanded=0;unsigned cases=0;
    for(int mode=0;mode<2;mode++){
        direct_storage_decode=mode!=0;
        for(int stage=0;stage<6;stage++)for(int slot=0;slot<19;slot++){
            if(slot==0&&stage!=0&&stage!=1&&stage!=5)continue;
            auto reference=constant_bank(stage,slot,shader);
            for(unsigned offset: {0u,4u,8u,12u,16u,24u,28u,0x10cu,0x110u,0x118u,0x120u,0x15cu,0x164u,0x168u,0x308u,0x310u,0x408u,0x510u,0x564u,0x568u,0x960u,65524u}){
                ShaderResource resource{};resource.cb_slot=slot;resource.cb_offset=offset/4;
                uint64_t address;uint32_t size;memcpy(&address,reference.data()+offset,8);memcpy(&size,reference.data()+offset+8,4);
                auto before=constant_bank_expanded_bytes;auto result=storage_binding(stage,resource,shader);
                if(result.address!=address||result.size!=size)throw std::runtime_error("constant descriptor bytes differ from padded bank");
                if(mode)expanded+=constant_bank_expanded_bytes-before;
                else if(constant_bank_expanded_bytes-before!=65536)throw std::runtime_error("constant decode negative control did not expand");
                ++cases;
            }
        }
        // No address-only cache: edits and shrinkage must become visible.
        ShaderResource resource{};resource.cb_slot=3;resource.cb_offset=0;
        for(unsigned size: {13u,8u,7u,1u,0u}){
            bytes[0]^=0x5a;render_state.uniforms[0][0].size=size;
            auto reference=constant_bank(0,3,shader);auto result=storage_binding(0,resource,shader);
            if(memcmp(&result.address,reference.data(),8)||memcmp(&result.size,reference.data()+8,4))throw std::runtime_error("constant mutation/shrinkage is stale");
        }
    }
    render_state=saved;direct_storage_decode=true;
    printf("CONSTANT_DECODE cases=%u fast_expanded_bytes=%llu\n",cases,(unsigned long long)expanded);
    if(expanded)throw std::runtime_error("storage descriptor still expands a 64 KiB bank");
    return 1;
}
