extern "C" uint64_t vkhost_test_dynamic_depth_bounds(int);
static void depth_bounds_contract(NVNqueue& queue,int dynamic,int preparation){
    vkhost_test_prepare_reuse(preparation);
    auto before=vkhost_test_dynamic_depth_bounds(dynamic);
    std::array<std::array<uint8_t,0x880>,2> control{};uint64_t code[2]{};NVNshaderData sd[2]{};
    for(unsigned i=0;i<2;i++){uint32_t magic=0x98761234,size=8;memcpy(control[i].data(),&magic,4);memcpy(control[i].data()+0x704,&size,4);memcpy(control[i].data()+0x714,&i,4);control[i][0x40]=93;sd[i]={reinterpret_cast<uint64_t>(&code[i]),control[i].data()};}
    vkhost_test_state_program(sd);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,2,sd);
    NVNtextureBuilder tb{};NVNtexture color{},depth{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,16,2);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);P(nvnTextureInitialize)(&color,&tb);
    P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH24_STENCIL8);P(nvnTextureInitialize)(&depth,&tb);
    NVNpolygonState polygon{};P(nvnPolygonStateSetDefaults)(&polygon);P(nvnPolygonStateSetCullFace)(&polygon,NVN_FACE_NONE);
    NVNcolorState cs{};NVNchannelMaskState mask{};NVNdepthStencilState ds{};
    P(nvnColorStateSetDefaults)(&cs);P(nvnChannelMaskStateSetDefaults)(&mask);P(nvnDepthStencilStateSetDefaults)(&ds);
    P(nvnDepthStencilStateSetDepthTestEnable)(&ds,1);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);P(nvnDepthStencilStateSetDepthFunc)(&ds,NVN_DEPTH_FUNC_ALWAYS);
    // Incoming .85 differs from stored .5: bounds must inspect the attachment,
    // not FragCoord.z. Alternating ranges distinguish these two semantics.
    alignas(16) std::array<float,8> red{1,0,0,1,.85f},green{0,1,0,1,.85f};float black[4]{},blue[4]{0,0,1,1};
    NVNtexture* targets[]={&color};NVNcopyRegion region{0,0,0,16,2,1};std::array<uint8_t,144> pixels{};Commands c;
    auto uniform=[&](auto& v){for(auto stage:{NVN_SHADER_STAGE_VERTEX,NVN_SHADER_STAGE_FRAGMENT})P(nvnCommandBufferBindUniformBuffer)(&c.buffer,stage,0,reinterpret_cast<uint64_t>(v.data()),sizeof(v));};
    auto draw=[&]{P(nvnCommandBufferDrawArrays)(&c.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);};
    auto begin=[&]{c.begin();P(nvnCommandBufferSetRenderTargets)(&c.buffer,1,targets,nullptr,&depth,nullptr);P(nvnCommandBufferBindProgram)(&c.buffer,&program,3);
        P(nvnCommandBufferBindPolygonState)(&c.buffer,&polygon);P(nvnCommandBufferBindColorState)(&c.buffer,&cs);P(nvnCommandBufferBindChannelMaskState)(&c.buffer,&mask);P(nvnCommandBufferBindDepthStencilState)(&c.buffer,&ds);
        P(nvnCommandBufferSetViewport)(&c.buffer,0,0,16,2);P(nvnCommandBufferSetScissor)(&c.buffer,0,0,16,2);P(nvnCommandBufferClearTexture)(&c.buffer,&color,nullptr,&region,black,15);P(nvnCommandBufferClearDepthStencil)(&c.buffer,.5f,1,0,255);};
    auto finish=[&]{pixels.fill(0xcc);P(nvnCommandBufferCopyTextureToBuffer)(&c.buffer,&color,nullptr,&region,reinterpret_cast<uint64_t>(pixels.data()),0);auto h=c.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);for(unsigned i=128;i<pixels.size();++i)require(pixels[i]==0xcc,"depth bounds readback guards");};
    // Unique ranges must change clipping without creating a pipeline per range.
    // A masked clear deliberately binds a static pipeline between every draw.
    for(unsigned round=0;round<2;++round){begin();uniform(red);
        for(unsigned i=0;i<32;++i){P(nvnCommandBufferClearTexture)(&c.buffer,&color,nullptr,&region,blue,4);P(nvnCommandBufferSetScissor)(&c.buffer,i%16,i/16,1,1);
            float minimum=(i&1?.7f:.1f)+i*.001f+round*.0001f;P(nvnCommandBufferSetDepthBounds)(&c.buffer,1,minimum,.95f);draw();}
        finish();for(unsigned i=0;i<32;++i)require(pixels[4*i]==(i&1?0:255)&&pixels[4*i+1]==0,"dynamic range accepts and rejects the same stored depth after clear and fresh recording");
    }
    auto created=vkhost_test_dynamic_depth_bounds(dynamic)-before;
    if(!getenv("OT2_PIPELINE_TEST_ROOT"))require(dynamic?created<=4:created>=64,"depth ranges share dynamic pipeline; static negative control creates distinct pipelines");
    // The enable bit remains immutable state and must not be dropped with the range.
    begin();P(nvnCommandBufferSetDepthBounds)(&c.buffer,0,.9f,1.f);uniform(green);draw();finish();
    for(unsigned i=0;i<32;++i)require(pixels[4*i]==0&&pixels[4*i+1]==255,"disabling depth bounds preserves all fragments");
    printf("DEPTH_BOUNDS_CONTRACT dynamic=%d preparation=%d created=%llu\n",dynamic,preparation,(unsigned long long)created);
    P(nvnProgramFinalize)(&program);P(nvnTextureFinalize)(&color);P(nvnTextureFinalize)(&depth);vkhost_test_dynamic_depth_bounds(1);vkhost_test_prepare_reuse(1);
}
