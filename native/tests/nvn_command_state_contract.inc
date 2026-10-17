extern "C" uint64_t vkhost_test_command_state(int,int);
extern "C" void vkhost_test_vertex_program(const NVNshaderData*);
extern "C" uint64_t vkhost_test_index_cache(int);
static void command_state_contract(NVNqueue& queue,int enabled){
    std::array<uint64_t,6> before;for(unsigned i=0;i<6;i++)before[i]=vkhost_test_command_state(enabled,i);
    vkhost_test_binding_cache(1);
    std::array<std::array<uint8_t,0x880>,2> control{};uint64_t code[2]{};NVNshaderData sd[2]{};
    for(unsigned i=0;i<2;i++){uint32_t magic=0x98761234,size=8;memcpy(control[i].data(),&magic,4);memcpy(control[i].data()+0x704,&size,4);memcpy(control[i].data()+0x714,&i,4);control[i][0x40]=79;sd[i]={reinterpret_cast<uint64_t>(&code[i]),control[i].data()};}
    vkhost_test_vertex_program(sd);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,2,sd);
    NVNtextureBuilder tb{};NVNtexture color{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,8,4);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);P(nvnTextureInitialize)(&color,&tb);
    NVNpolygonState polygon{};P(nvnPolygonStateSetDefaults)(&polygon);P(nvnPolygonStateSetCullFace)(&polygon,NVN_FACE_NONE);
    NVNcolorState cs{};NVNdepthStencilState ds{};NVNchannelMaskState mask{};P(nvnColorStateSetDefaults)(&cs);P(nvnDepthStencilStateSetDefaults)(&ds);P(nvnChannelMaskStateSetDefaults)(&mask);
    NVNvertexAttribState attribute{};P(nvnVertexAttribStateSetDefaults)(&attribute);P(nvnVertexAttribStateSetFormat)(&attribute,NVN_FORMAT_RG32F,0);P(nvnVertexAttribStateSetStreamIndex)(&attribute,7);
    std::array<NVNvertexStreamState,8> streams;for(auto& s:streams){P(nvnVertexStreamStateSetDefaults)(&s);P(nvnVertexStreamStateSetStride)(&s,8);}
    std::array<float,6> full{-1,-1,3,-1,-1,3},outside{5,5,7,5,5,7};std::array<uint16_t,3> index16{0,1,2};std::array<uint32_t,3> index32{0,1,2},degenerate{0,0,0};
    alignas(16) std::array<float,8> red{1,0,0,1},green{0,1,0,1};float blue[4]{0,0,1,1},black[4]{};
    NVNtexture* targets[]={&color};NVNcopyRegion region{0,0,0,8,4,1};std::array<uint8_t,144> pixels;Commands c;
    auto vertices=[&](auto& v){P(nvnCommandBufferBindVertexBuffer)(&c.buffer,7,reinterpret_cast<uint64_t>(v.data()),sizeof(v));};
    auto uniform=[&](auto& v){P(nvnCommandBufferBindUniformBuffer)(&c.buffer,NVN_SHADER_STAGE_FRAGMENT,0,reinterpret_cast<uint64_t>(v.data()),sizeof(v));};
    auto draw=[&](auto& index,NVNindexType type){P(nvnCommandBufferDrawElements)(&c.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,type,3,reinterpret_cast<uint64_t>(index.data()));};
    auto begin=[&]{c.begin();P(nvnCommandBufferSetRenderTargets)(&c.buffer,1,targets,nullptr,nullptr,nullptr);P(nvnCommandBufferBindProgram)(&c.buffer,&program,3);
        P(nvnCommandBufferBindPolygonState)(&c.buffer,&polygon);P(nvnCommandBufferBindColorState)(&c.buffer,&cs);P(nvnCommandBufferBindDepthStencilState)(&c.buffer,&ds);P(nvnCommandBufferBindChannelMaskState)(&c.buffer,&mask);
        P(nvnCommandBufferBindVertexAttribState)(&c.buffer,1,&attribute);P(nvnCommandBufferBindVertexStreamState)(&c.buffer,streams.size(),streams.data());
        P(nvnCommandBufferSetViewport)(&c.buffer,0,0,8,4);P(nvnCommandBufferSetScissor)(&c.buffer,0,0,8,4);P(nvnCommandBufferClearTexture)(&c.buffer,&color,nullptr,&region,black,15);vertices(full);};
    auto finish=[&]{pixels.fill(0xcc);P(nvnCommandBufferCopyTextureToBuffer)(&c.buffer,&color,nullptr,&region,reinterpret_cast<uint64_t>(pixels.data()),0);auto h=c.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);for(unsigned i=128;i<pixels.size();i++)require(pixels[i]==0xcc,"state cache readback guard");};
    // Identical draws exercise every binding kind; masked draw-clear overwrites
    // graphics pipeline, viewport and scissor inside the same recording.
    begin();P(nvnCommandBufferSetScissor)(&c.buffer,0,0,4,4);uniform(red);draw(index16,NVN_INDEX_TYPE_UNSIGNED_SHORT);draw(index16,NVN_INDEX_TYPE_UNSIGNED_SHORT);
    P(nvnCommandBufferClearTexture)(&c.buffer,&color,nullptr,&region,blue,4);uniform(green);draw(index16,NVN_INDEX_TYPE_UNSIGNED_SHORT);finish();
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<8;x++){auto i=(y*8+x)*4;require(pixels[i]==0&&pixels[i+1]==(x<4?255:0)&&pixels[i+2]==(x<4?0:255),"draw-clear restores pipeline, negative viewport and prior scissor");}
    // A new command buffer must bind again, even when the driver recycles its
    // handle. Vertex ranges, index sizes and offsets cannot reuse stale state.
    for(unsigned round=0;round<4;round++){
        begin();uniform(green);draw(index16,NVN_INDEX_TYPE_UNSIGNED_SHORT);uniform(red);vertices(outside);draw(index16,NVN_INDEX_TYPE_UNSIGNED_SHORT);
        vertices(full);draw(degenerate,NVN_INDEX_TYPE_UNSIGNED_INT);finish();
        for(unsigned i=0;i<32;i++)require(pixels[i*4]==0&&pixels[i*4+1]==255,"changed vertex/index offsets and types prevent stale geometry");
        begin();uniform(red);draw(index32,NVN_INDEX_TYPE_UNSIGNED_INT);draw(index32,NVN_INDEX_TYPE_UNSIGNED_INT);finish();
        for(unsigned i=0;i<32;i++)require(pixels[i*4]==255&&pixels[i*4+1]==0,"fresh recording restores descriptors and valid uint32 index binding");
    }
    alignas(4096) std::array<uint8_t,4096> indices{};NVNmemoryPool index_pool{};NVNmemoryPoolBuilder pb{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,indices.data(),indices.size());
    require(P(nvnMemoryPoolInitialize)(&index_pool,&pb),"tracked index pool");
    auto index_hits=vkhost_test_index_cache(enabled);
    for(unsigned type=0;type<3;++type){
        auto set_indices=[&](bool full_geometry,unsigned base){for(unsigned i=0;i<3;++i){uint32_t v=full_geometry?i+base:0;memcpy(indices.data()+(i<<type),&v,size_t(1)<<type);}};
        auto indexed=[&](int base){P(nvnCommandBufferDrawElementsBaseVertex)(&c.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,NVNindexType(type),3,reinterpret_cast<uint64_t>(indices.data()),base);};
        set_indices(false,0);begin();uniform(red);indexed(0);finish();
        for(unsigned i=0;i<32;i++)require(pixels[4*i]==0,"degenerate tracked indices");
        set_indices(true,0);begin();uniform(green);indexed(0);indexed(0);finish();
        for(unsigned i=0;i<32;i++)require(pixels[4*i+1]==255,"CPU mutation invalidates cached index extent");
        set_indices(true,1);begin();uniform(red);indexed(-1);indexed(-1);finish();
        for(unsigned i=0;i<32;i++)require(pixels[4*i]==255,"cached maximum preserves negative base vertex");
        P(nvnMemoryPoolFinalize)(&index_pool);memset(indices.data(),0,indices.size());
        require(P(nvnMemoryPoolInitialize)(&index_pool,&pb),"same-address index pool recreation");
    }
    auto hit_delta=vkhost_test_index_cache(enabled)-index_hits;
    require(enabled?hit_delta>=6:hit_delta==0,"index summary hits and disabled negative control");
    printf("INDEX_SUMMARY_CONTRACT enabled=%d hits=%llu\n",enabled,(unsigned long long)hit_delta);
    P(nvnMemoryPoolFinalize)(&index_pool);vkhost_test_index_cache(1);
    for(unsigned i=0;i<6;i++){auto delta=vkhost_test_command_state(enabled,i)-before[i];require(enabled?delta>0:delta==0,"state filter exercised for all six kinds with disabled negative control");printf("COMMAND_STATE_CONTRACT enabled=%d kind=%u skipped=%llu\n",enabled,i,(unsigned long long)delta);}
    P(nvnProgramFinalize)(&program);P(nvnTextureFinalize)(&color);
    puts("PASS command binding state: duplicates, clear interleave, descriptor versions, vertex/index mutations and fresh recordings");
}
