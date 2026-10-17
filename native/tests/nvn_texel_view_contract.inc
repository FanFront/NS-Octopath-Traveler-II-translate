extern "C" void vkhost_test_texel_program(const NVNshaderData*);
extern "C" uint64_t vkhost_test_texel_cache(int,int,size_t);
extern "C" int vkhost_test_texel_cache_lifetime();
extern "C" int vkhost_test_cache_pressure(int);
extern "C" uint64_t vkhost_test_texel_descriptors(int);
static void texel_view_contract(NVNqueue& queue,bool specialize=false){
    require(vkhost_test_texel_cache_lifetime(),"texel view full key, bounded live leases, eviction and buffer lifetime");
    std::array<uint8_t,0x880> control{};uint64_t code=0;uint32_t magic=0x98761234,size=8,stage=5;
    memcpy(control.data(),&magic,4);memcpy(control.data()+0x704,&size,4);memcpy(control.data()+0x714,&stage,4);control[0x40]=specialize?88:87;
    NVNshaderData shader{reinterpret_cast<uint64_t>(&code),control.data()};vkhost_test_texel_program(&shader);
    NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,1,&shader);
    alignas(4096) std::array<uint8_t,65536> descriptors{};
    alignas(4096) std::array<uint32_t,2048> source{};
    alignas(256) std::array<uint32_t,16> output{};
    NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{},data_pool{};NVNtexturePool texture_pool{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,descriptors.data(),descriptors.size());P(nvnMemoryPoolInitialize)(&pool,&pb);
    P(nvnTexturePoolInitialize)(&texture_pool,&pool,0,1024);
    P(nvnMemoryPoolBuilderSetStorage)(&pb,source.data(),sizeof(source));P(nvnMemoryPoolInitialize)(&data_pool,&pb);
    NVNtextureBuilder tb{};NVNtexture textures[3]{};P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_BUFFER);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_R32UI);
    for(unsigned i=0;i<3;++i){P(nvnTextureBuilderSetWidth)(&tb,i==2?8:4);P(nvnTextureBuilderSetStorage)(&tb,&data_pool,i==1?256:0);
        require(P(nvnTextureInitialize)(&textures[i],&tb),"texel test texture");P(nvnTexturePoolRegisterTexture)(&texture_pool,256+i,&textures[i],nullptr);}
    Commands c;
    auto record=[&](Commands& cmd,unsigned image,uint32_t* result){cmd.begin();P(nvnCommandBufferBindProgram)(&cmd.buffer,&program,NVN_SHADER_STAGE_COMPUTE_BIT);
        P(nvnCommandBufferSetTexturePool)(&cmd.buffer,&texture_pool);P(nvnCommandBufferBindTexture)(&cmd.buffer,NVN_SHADER_STAGE_COMPUTE,0,256+image);
        P(nvnCommandBufferBindStorageBuffer)(&cmd.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(result),16);
        for(unsigned n=0;n<8;++n)P(nvnCommandBufferDispatchCompute)(&cmd.buffer,1,1,1);return cmd.end();};
    for(int retained:{0,1})for(int enabled:{0,1}){
        const auto descriptor_reuse=vkhost_test_texel_descriptors(retained);
        const auto hits=vkhost_test_texel_cache(enabled,0,8192),creates=vkhost_test_texel_cache(enabled,1,8192);
        for(unsigned round=0;round<12;++round){
            for(unsigned i=0;i<source.size();++i)source[i]=round*100+i;
            output.fill(0xabcdef);const auto image=round%3;const auto h=record(c,image,output.data());P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
            for(unsigned i=0;i<4;++i)require(output[i]==source[(image==1?64:0)+i]+17,"warm texel view sees current contents, offset and range");
            for(unsigned i=4;i<output.size();++i)require(output[i]==0xabcdef,"texel readback guards");
        }
        const auto gained=vkhost_test_texel_cache(enabled,0,8192)-hits,created=vkhost_test_texel_cache(enabled,1,8192)-creates;
        require(enabled?(gained>=84&&created<=12):(gained==0&&created==96),"texel cache reuses real draw views; disabled negative control creates each view");
        printf("TEXEL_CACHE enabled=%d hits=%llu creates=%llu\n",enabled,(unsigned long long)gained,(unsigned long long)created);
        const auto saved=vkhost_test_texel_descriptors(retained)-descriptor_reuse;
        require(retained&&enabled?saved>0:saved==0,"only leased cached texel views retain descriptor contents/deltas");
        printf("TEXEL_DESCRIPTORS retained=%d cached=%d reused=%llu\n",retained,enabled,(unsigned long long)saved);
    }
    {
        vkhost_test_texel_cache(1,0,1);
        alignas(256) std::array<std::array<uint32_t,64>,3> results{};
        std::array<Commands,3> commands;
        uint64_t handles[3];for(unsigned i=0;i<3;++i){results[i].fill(0xabcdef);handles[i]=record(commands[i],i,results[i].data());}
        vkhost_test_hold_gpu(1);const auto before=vkhost_test_encoded_count();
        for(auto h:handles)P(nvnQueueSubmitCommands)(&queue,1,&h);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(vkhost_test_encoded_count()-before<3&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        const auto encoded=vkhost_test_encoded_count()-before;
        bool unpublished=true;for(const auto& block:results)for(auto value:block)unpublished&=value==0xabcdef;
        const bool pressure_safe=vkhost_test_cache_pressure(1);
        vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);
        const bool retired=vkhost_test_cache_pressure(2);vkhost_test_cache_pressure(0);
        require(pressure_safe&&retired,"zero retention budget preserves pending GPU leases and reclaims all retired descriptors/views/buffers");
        require(encoded==3&&unpublished,"held GPU allows three slots without evicting leased view or publishing results");
        for(unsigned b=0;b<3;++b)for(unsigned i=0;i<64;++i)
            require(results[b][i]==(i<4?source[(b==1?64:0)+i]+17:0xabcdef),"full leased texel cache fallback preserves GPU values and guards");
        vkhost_test_texel_cache(1,0,8192);
    }
    if(specialize){
        // The same shader and texture change numeric view type. Both cached
        // descriptor preparation and pipeline lookup must follow that view.
        for(int f:{NVN_FORMAT_R32UI,NVN_FORMAT_R16UI,NVN_FORMAT_R32F,NVN_FORMAT_R32UI}){
            NVNtextureView view{};P(nvnTextureViewSetDefaults)(&view);P(nvnTextureViewSetFormat)(&view,NVNformat(f));P(nvnTexturePoolRegisterTexture)(&texture_pool,256,&textures[0],&view);
            output.fill(0xabcdef);auto h=record(c,0,output.data());P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
            for(unsigned i=0;i<4;i++)require(output[i]==(f==NVN_FORMAT_R16UI?reinterpret_cast<uint16_t*>(source.data())[i]:source[i])+17,"integer specialization follows changing numeric views and raw float register bits");
            for(unsigned i=4;i<output.size();i++)require(output[i]==0xabcdef,"specialized texel output guards");
        }
        puts("PASS frontend integer texel specialization and numeric view replacement");
    }
    for(auto& t:textures)P(nvnTextureFinalize)(&t);
    P(nvnTexturePoolFinalize)(&texture_pool);P(nvnMemoryPoolFinalize)(&pool);P(nvnMemoryPoolFinalize)(&data_pool);P(nvnProgramFinalize)(&program);
    puts("PASS real GPU texel cache: warm changed contents, offset/range, guards, disabled control and pressure during pending work");
}
