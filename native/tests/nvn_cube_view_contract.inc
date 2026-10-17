extern "C" void vkhost_test_cube_program(const NVNshaderData*,int);
static void cube_view_contract(NVNqueue& queue){
    std::array<std::array<uint8_t,0x880>,2> controls{};uint64_t code=0;NVNprogram programs[2]{};
    for(unsigned i=0;i<2;i++){
        auto& c=controls[i];uint32_t magic=0x98761234,size=8,stage=5;
        memcpy(c.data(),&magic,4);memcpy(c.data()+0x704,&size,4);memcpy(c.data()+0x714,&stage,4);c[0x40]=92+i;
        NVNshaderData shader{reinterpret_cast<uint64_t>(&code),c.data()};vkhost_test_cube_program(&shader,i);
        P(nvnProgramInitialize)(&programs[i],&device);P(nvnProgramSetShaders)(&programs[i],1,&shader);
    }
    alignas(4096) std::array<uint8_t,65536> descriptors{};
    NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};NVNtexturePool texture_pool{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,descriptors.data(),descriptors.size());P(nvnMemoryPoolInitialize)(&pool,&pb);
    P(nvnTexturePoolInitialize)(&texture_pool,&pool,0,1024);
    NVNtextureBuilder tb{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_CUBEMAP);
    P(nvnTextureBuilderSetSize2D)(&tb,4,4);P(nvnTextureBuilderSetLevels)(&tb,2);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    NVNtexture texture{};require(P(nvnTextureInitialize)(&texture,&tb),"cube texture");
    Commands commands;
    auto submit=[&](){auto h=commands.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);};
    alignas(256) std::array<float,52> output{};
    // Two differently colored mip levels; every face and channel is distinct.
    for(unsigned revision=0;revision<2;revision++){
        std::array<std::vector<uint8_t>,2> pixels;
        commands.begin();
        for(unsigned level=0;level<2;level++){
            unsigned side=4>>level;pixels[level].resize(side*side*6*4);
            for(unsigned face=0;face<6;face++)for(unsigned pixel=0;pixel<side*side;pixel++)for(unsigned channel=0;channel<4;channel++)
                pixels[level][(face*side*side+pixel)*4+channel]=20+revision*70+level*30+face*4+channel;
            NVNtextureView view{};P(nvnTextureViewSetDefaults)(&view);P(nvnTextureViewSetLevels)(&view,level,1);
            NVNcopyRegion region{0,0,0,int(side),int(side),6};
            P(nvnCommandBufferCopyBufferToTexture)(&commands.buffer,reinterpret_cast<uint64_t>(pixels[level].data()),&texture,&view,&region,0);
        }
        submit();
        for(unsigned restricted=0;restricted<2;restricted++){
            NVNtextureView view{};P(nvnTextureViewSetDefaults)(&view);
            if(restricted){P(nvnTextureViewSetLevels)(&view,1,1);P(nvnTextureViewSetLayers)(&view,0,6);
                P(nvnTextureViewSetSwizzle)(&view,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_G,NVN_TEXTURE_SWIZZLE_R,NVN_TEXTURE_SWIZZLE_A);}
            P(nvnTexturePoolRegisterTexture)(&texture_pool,256,&texture,restricted?&view:nullptr);
            // Same descriptor alternates shaders without being re-registered:
            // its cached cube view must not be reused for the array shader.
            for(unsigned array:{0,1,0,1}){
                output.fill(-12345);commands.begin();
                P(nvnCommandBufferBindProgram)(&commands.buffer,&programs[array],NVN_SHADER_STAGE_COMPUTE_BIT);
                P(nvnCommandBufferSetTexturePool)(&commands.buffer,&texture_pool);
                P(nvnCommandBufferBindTexture)(&commands.buffer,NVN_SHADER_STAGE_COMPUTE,0,256);
                P(nvnCommandBufferBindStorageBuffer)(&commands.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(output.data()),48*sizeof(float));
                P(nvnCommandBufferDispatchCompute)(&commands.buffer,1,1,1);submit();
                for(unsigned level=0;level<2;level++)for(unsigned face=0;face<6;face++)for(unsigned channel=0;channel<4;channel++){
                    unsigned component=restricted&&channel!=3?2-channel:channel;
                    float expected=(20+revision*70+(restricted?1:level)*30+face*4+component)/255.0f;
                    require(std::abs(output[(level*6+face)*4+channel]-expected)<.0001f,"cube/array shader faces, mip range, swizzle and updated content");
                }
                for(unsigned i=48;i<output.size();i++)require(output[i]==-12345,"cube view output guards");
            }
        }
    }
    P(nvnTextureFinalize)(&texture);P(nvnTexturePoolFinalize)(&texture_pool);P(nvnMemoryPoolFinalize)(&pool);
    for(auto& p:programs)P(nvnProgramFinalize)(&p);
    puts("PASS frontend cube-to-array sampling: six faces, mip/view range, swizzle, alternating shaders, content updates and guards");
}
