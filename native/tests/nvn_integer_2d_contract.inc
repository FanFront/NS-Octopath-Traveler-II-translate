extern "C" void vkhost_test_depth_storage_counts(uint64_t*);
static void depth_alias_contract(NVNqueue& queue){
    std::array<uint64_t,3> before{},after{};vkhost_test_depth_storage_counts(before.data());
    alignas(4096) std::array<uint8_t,16384> storage{};NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,storage.data(),storage.size());P(nvnMemoryPoolInitialize)(&pool,&pb);
    NVNtextureBuilder tb{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,4,1);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH24_STENCIL8);P(nvnTextureBuilderSetStorage)(&tb,&pool,0);
    NVNtexture writer{},reader{},later{},separate{};P(nvnTextureInitialize)(&writer,&tb);P(nvnTextureInitialize)(&reader,&tb);
    P(nvnTextureBuilderSetStorage)(&tb,&pool,4096);P(nvnTextureInitialize)(&separate,&tb);
    const std::array<uint32_t,4> values{0x10800000,0x30800000,0x70800000,0x00800000},others{0x40800000,0x60800000,0x20800000,0x50800000};NVNcopyRegion region{0,0,0,4,1,1};
    {Commands c;c.begin();P(nvnCommandBufferCopyBufferToTexture)(&c.buffer,reinterpret_cast<uint64_t>(values.data()),&writer,nullptr,&region,0);
        P(nvnCommandBufferCopyBufferToTexture)(&c.buffer,reinterpret_cast<uint64_t>(others.data()),&separate,nullptr,&region,0);auto h=c.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);}
    // The reader has never been used by the backend when the writer is freed.
    P(nvnTextureFinalize)(&writer);
    auto check=[&](NVNtexture& t,const auto& expected){std::array<uint32_t,4> got{};Commands c;c.begin();P(nvnCommandBufferCopyTextureToBuffer)(&c.buffer,&t,nullptr,&region,reinterpret_cast<uint64_t>(got.data()),0);auto h=c.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);require(got==expected,"depth alias contents survive writer release and independent offsets");};
    check(reader,values);check(separate,others);
    P(nvnTextureBuilderSetStorage)(&tb,&pool,0);P(nvnTextureInitialize)(&later,&tb);P(nvnTextureFinalize)(&reader);check(later,values);
    P(nvnTextureFinalize)(&later);P(nvnTextureFinalize)(&separate);P(nvnMemoryPoolFinalize)(&pool);vkhost_test_depth_storage_counts(after.data());
    require(before==after,"depth alias metadata and GPU storage reclaimed at final owner release");
    puts("PASS actual frontend depth aliases: unused reader, both creation orders, writer release, independent offset and final reclamation");
}
extern "C" void vkhost_test_integer_2d_program(const NVNshaderData*);
static void integer_2d_contract(NVNqueue& queue){
    std::array<uint8_t,0x880> control{};uint64_t code=0;uint32_t magic=0x98761234,size=8,stage=5;
    memcpy(control.data(),&magic,4);memcpy(control.data()+0x704,&size,4);memcpy(control.data()+0x714,&stage,4);control[0x40]=94;
    NVNshaderData shader{reinterpret_cast<uint64_t>(&code),control.data()};vkhost_test_integer_2d_program(&shader);
    NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,1,&shader);
    alignas(4096) std::array<uint8_t,65536> descriptors{};NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};NVNtexturePool texture_pool{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,descriptors.data(),descriptors.size());P(nvnMemoryPoolInitialize)(&pool,&pb);P(nvnTexturePoolInitialize)(&texture_pool,&pool,0,1024);
    Commands commands;alignas(256) std::array<uint32_t,20> output{};
    bool pass=true;
    for(int format:{NVN_FORMAT_R16UI,NVN_FORMAT_R32UI,NVN_FORMAT_DEPTH24_STENCIL8,NVN_FORMAT_DEPTH32F_STENCIL8}){
        NVNtextureBuilder tb{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,4,1);P(nvnTextureBuilderSetFormat)(&tb,NVNformat(format));
        NVNtexture t{};require(P(nvnTextureInitialize)(&t,&tb),"integer/stencil source");
        bool stencil=format==NVN_FORMAT_DEPTH24_STENCIL8||format==NVN_FORMAT_DEPTH32F_STENCIL8;
        NVNtextureView view{};P(nvnTextureViewSetDefaults)(&view);if(stencil)P(nvnTextureViewSetDepthStencilMode)(&view,NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
        P(nvnTexturePoolRegisterTexture)(&texture_pool,256,&t,&view);
        std::array<uint32_t,4> values{1,37,127,255},words{};std::array<uint16_t,4> shorts{};std::array<uint32_t,8> wide{};
        for(unsigned i=0;i<4;i++){shorts[i]=values[i];words[i]=stencil?(values[i]<<24)|0x800000:values[i];wide[2*i]=0x3f000000;wide[2*i+1]=values[i];}
        void* source=format==NVN_FORMAT_R16UI?static_cast<void*>(shorts.data()):format==NVN_FORMAT_DEPTH32F_STENCIL8?static_cast<void*>(wide.data()):static_cast<void*>(words.data());
        NVNcopyRegion region{0,0,0,4,1,1};output.fill(0xcccccccc);commands.begin();
        P(nvnCommandBufferCopyBufferToTexture)(&commands.buffer,reinterpret_cast<uint64_t>(source),&t,nullptr,&region,0);
        P(nvnCommandBufferBindProgram)(&commands.buffer,&program,NVN_SHADER_STAGE_COMPUTE_BIT);P(nvnCommandBufferSetTexturePool)(&commands.buffer,&texture_pool);P(nvnCommandBufferBindTexture)(&commands.buffer,NVN_SHADER_STAGE_COMPUTE,0,256);
        P(nvnCommandBufferBindStorageBuffer)(&commands.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(output.data()),64);P(nvnCommandBufferDispatchCompute)(&commands.buffer,1,1,1);
        auto h=commands.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
        bool ok=true;for(unsigned i=0;i<4;i++)ok&=output[i*4+(stencil?1:0)]==values[i];for(unsigned i=16;i<20;i++)ok&=output[i]==0xcccccccc;pass&=ok;
        printf("INTEGER_2D format=%d rgba=%u,%u,%u,%u pass=%d\n",format,output[4],output[5],output[6],output[7],ok);P(nvnTextureFinalize)(&t);
    }
    P(nvnTexturePoolFinalize)(&texture_pool);P(nvnMemoryPoolFinalize)(&pool);P(nvnProgramFinalize)(&program);
    require(pass,"integer 2D values and Maxwell stencil green component preserve register bits");
}
extern "C" void vkhost_test_lighting_mask_program(const NVNshaderData*);
static void lighting_mask_contract(NVNqueue& queue){
    std::array<std::array<uint8_t,0x880>,2> controls{};uint64_t code[2]{};NVNshaderData shaders[2]{};
    for(unsigned i=0;i<2;i++){auto& c=controls[i];uint32_t magic=0x98761234,size=8;memcpy(c.data(),&magic,4);memcpy(c.data()+0x704,&size,4);memcpy(c.data()+0x714,&i,4);c[0x40]=96;shaders[i]={reinterpret_cast<uint64_t>(&code[i]),c.data()};}
    vkhost_test_lighting_mask_program(shaders);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,2,shaders);
    alignas(4096) std::array<uint8_t,65536> descriptors{};NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};NVNtexturePool texture_pool{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,descriptors.data(),descriptors.size());P(nvnMemoryPoolInitialize)(&pool,&pb);P(nvnTexturePoolInitialize)(&texture_pool,&pool,0,1024);
    NVNtextureBuilder tb{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,4,1);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH24_STENCIL8);NVNmemoryPool data_pool{};alignas(4096) std::array<uint8_t,16384> storage{};
    P(nvnMemoryPoolBuilderSetStorage)(&pb,storage.data(),storage.size());P(nvnMemoryPoolInitialize)(&data_pool,&pb);P(nvnTextureBuilderSetStorage)(&tb,&data_pool,0);
    NVNtexture depth{},alias{};P(nvnTextureInitialize)(&depth,&tb);P(nvnTextureInitialize)(&alias,&tb);
    NVNtextureView stencil{};P(nvnTextureViewSetDefaults)(&stencil);P(nvnTextureViewSetDepthStencilMode)(&stencil,NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);P(nvnTexturePoolRegisterTexture)(&texture_pool,256,&alias,&stencil);
    NVNpolygonState polygon{};P(nvnPolygonStateSetDefaults)(&polygon);P(nvnPolygonStateSetCullFace)(&polygon,NVN_FACE_NONE);
    NVNcolorState cs{};NVNchannelMaskState mask{};NVNdepthStencilState ds{};P(nvnColorStateSetDefaults)(&cs);P(nvnChannelMaskStateSetDefaults)(&mask);P(nvnDepthStencilStateSetDefaults)(&ds);
    const uint32_t values[]={0x00800000,0x10800000,0x30800000,0x70800000};const uint32_t expected[]={1,0,2,6};NVNcopyRegion region{0,0,0,4,1,1};Commands c;
    for(int format:{NVN_FORMAT_R16UI,NVN_FORMAT_R32UI,NVN_FORMAT_R16I,NVN_FORMAT_R16UI}){
        NVNtexture color{};P(nvnTextureBuilderSetStorage)(&tb,nullptr,0);P(nvnTextureBuilderSetFormat)(&tb,NVNformat(format));P(nvnTextureInitialize)(&color,&tb);NVNtexture* targets[]={&color};std::array<uint32_t,8> pixels;pixels.fill(0xcccccccc);
        c.begin();P(nvnCommandBufferCopyBufferToTexture)(&c.buffer,reinterpret_cast<uint64_t>(values),&depth,nullptr,&region,0);
        P(nvnCommandBufferSetRenderTargets)(&c.buffer,1,targets,nullptr,nullptr,nullptr);P(nvnCommandBufferSetViewport)(&c.buffer,0,0,4,1);P(nvnCommandBufferSetScissor)(&c.buffer,0,0,4,1);
        P(nvnCommandBufferBindPolygonState)(&c.buffer,&polygon);P(nvnCommandBufferBindColorState)(&c.buffer,&cs);P(nvnCommandBufferBindChannelMaskState)(&c.buffer,&mask);P(nvnCommandBufferBindDepthStencilState)(&c.buffer,&ds);P(nvnCommandBufferSetDepthBounds)(&c.buffer,0,0,1);
        P(nvnCommandBufferBindProgram)(&c.buffer,&program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);P(nvnCommandBufferSetTexturePool)(&c.buffer,&texture_pool);P(nvnCommandBufferBindTexture)(&c.buffer,NVN_SHADER_STAGE_FRAGMENT,0,256);
        P(nvnCommandBufferDrawArrays)(&c.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);P(nvnCommandBufferCopyTextureToBuffer)(&c.buffer,&color,nullptr,&region,reinterpret_cast<uint64_t>(pixels.data()),0);
        auto h=c.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
        for(unsigned i=0;i<4;i++){auto actual=format==NVN_FORMAT_R32UI?pixels[i]:reinterpret_cast<uint16_t*>(pixels.data())[i];require(actual==expected[i],"stencil to integer lighting mask retains channel bits");}
        for(unsigned i=4;i<pixels.size();i++)require(pixels[i]==0xcccccccc,"lighting mask output guard");P(nvnTextureFinalize)(&color);
    }
    P(nvnTextureFinalize)(&depth);P(nvnTextureFinalize)(&alias);P(nvnMemoryPoolFinalize)(&data_pool);P(nvnTexturePoolFinalize)(&texture_pool);P(nvnMemoryPoolFinalize)(&pool);P(nvnProgramFinalize)(&program);
    puts("PASS actual frontend stencil-to-light-mask: masks 1/0/2/6, signed and unsigned integer attachments, shader reuse and guards");
}
