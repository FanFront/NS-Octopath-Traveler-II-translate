// Exercise attachment preservation through the public recorder and real GPU.
// A read-only pass must preserve prior memory, and a later writer must never
// inherit STORE_OP_NONE from its predecessor. Run with the optimization off too.
extern "C" uint64_t vkhost_test_batch_work(unsigned);
extern "C" uint64_t vkhost_test_attachment_stores(int,int);
extern "C" void vkhost_test_attachment_program(const NVNshaderData*);
static void attachment_contract(NVNqueue& queue,int enabled,int width=8,int height=4,int level=1){
    vkhost_test_query_batching(1);vkhost_test_attachment_stores(enabled,0);
    std::array<std::array<uint8_t,0x880>,2> controls{};uint64_t code[2]{};NVNshaderData sd[2]{};
    for(unsigned i=0;i<2;++i){uint32_t magic=0x98761234,size=8;memcpy(controls[i].data(),&magic,4);memcpy(controls[i].data()+0x704,&size,4);memcpy(controls[i].data()+0x714,&i,4);sd[i]={reinterpret_cast<uint64_t>(&code[i]),controls[i].data()};}
    vkhost_test_state_program(sd);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,2,sd);
    NVNtextureBuilder tb{};NVNtexture color{},depth{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,width<<level,height<<level);P(nvnTextureBuilderSetLevels)(&tb,3);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);P(nvnTextureInitialize)(&color,&tb);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH24_STENCIL8);P(nvnTextureInitialize)(&depth,&tb);
    NVNtextureView mip{},range{};P(nvnTextureViewSetDefaults)(&mip);P(nvnTextureViewSetLevels)(&mip,level,1);range=mip;P(nvnTextureViewSetLevels)(&range,level,2);
    NVNtexture* targets[]={&color};const NVNtextureView* views[]={&mip};NVNcopyRegion region{0,0,0,width,height,1};const unsigned count=unsigned(width*height);
    NVNpolygonState polygon{};NVNcolorState cs{};NVNblendState blend{};NVNchannelMaskState mask{};NVNdepthStencilState ds{};
    P(nvnPolygonStateSetDefaults)(&polygon);P(nvnColorStateSetDefaults)(&cs);P(nvnBlendStateSetDefaults)(&blend);
    struct alignas(16) Values{float color[4],params[4];};Values red{{1,0,0,1},{.25f}},green{{0,1,0,1},{.75f}};
    std::vector<uint8_t> pixels(count*4+16);std::vector<uint32_t> depths(count+4);Commands cmd;
    auto bind=[&]{P(nvnCommandBufferBindDepthStencilState)(&cmd.buffer,&ds);P(nvnCommandBufferBindChannelMaskState)(&cmd.buffer,&mask);};
    auto begin=[&]{
        P(nvnDepthStencilStateSetDefaults)(&ds);P(nvnDepthStencilStateSetDepthTestEnable)(&ds,1);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);P(nvnChannelMaskStateSetDefaults)(&mask);
        cmd.begin();views[0]=&mip;P(nvnCommandBufferSetRenderTargets)(&cmd.buffer,1,targets,views,&depth,&mip);
        P(nvnCommandBufferBindProgram)(&cmd.buffer,&program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
        P(nvnCommandBufferBindPolygonState)(&cmd.buffer,&polygon);P(nvnCommandBufferBindColorState)(&cmd.buffer,&cs);P(nvnCommandBufferBindBlendState)(&cmd.buffer,&blend);bind();
        P(nvnCommandBufferSetViewport)(&cmd.buffer,0,0,width,height);P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,width,height);
        P(nvnCommandBufferSetStencilMask)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,255);P(nvnCommandBufferSetStencilValueMask)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,255);P(nvnCommandBufferSetStencilRef)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,9);
        float blue[4]{0,0,1,1};P(nvnCommandBufferClearTexture)(&cmd.buffer,&color,&mip,&region,blue,15);P(nvnCommandBufferClearDepthStencil)(&cmd.buffer,1,1,7,255);
    };
    auto draw=[&](Values& value){bind();for(auto stage:{NVN_SHADER_STAGE_VERTEX,NVN_SHADER_STAGE_FRAGMENT})P(nvnCommandBufferBindUniformBuffer)(&cmd.buffer,stage,0,reinterpret_cast<uint64_t>(&value),sizeof(value));P(nvnCommandBufferDrawArrays)(&cmd.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);};
    auto end=[&]{
        std::fill(pixels.begin(),pixels.end(),0xcc);std::fill(depths.begin(),depths.end(),0xcccccccc);P(nvnCommandBufferCopyTextureToBuffer)(&cmd.buffer,&color,&mip,&region,reinterpret_cast<uint64_t>(pixels.data()),0);P(nvnCommandBufferCopyTextureToBuffer)(&cmd.buffer,&depth,&mip,&region,reinterpret_cast<uint64_t>(depths.data()),0);
        auto h=cmd.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
        for(unsigned i=count*4;i<pixels.size();++i)require(pixels[i]==0xcc,"attachment color readback guard");for(unsigned i=count;i<depths.size();++i)require(depths[i]==0xcccccccc,"attachment depth readback guard");
    };
    auto check_pixels=[&](int r,int g,int b,uint32_t z,unsigned stencil){for(unsigned i=0;i<count;++i){require(pixels[4*i]==r&&pixels[4*i+1]==g&&pixels[4*i+2]==b&&pixels[4*i+3]==255,"attachment pass retains expected color");require(std::abs(int(depths[i]&0xffffff)-int(z))<=1&&(depths[i]>>24)==stencil,"attachment pass retains depth and stencil independently");}};
    auto stats=[&]{std::array<uint64_t,5> s{};for(int i=0;i<5;++i)s[i]=vkhost_test_attachment_stores(enabled,i);return s;};
    auto before=stats();auto passes=vkhost_test_render_passes();
    begin();draw(red);views[0]=&range;P(nvnCommandBufferSetRenderTargets)(&cmd.buffer,1,targets,views,&depth,&range);draw(green);end();check_pixels(0,255,0,0xffffff,7);
    auto after=stats();require(vkhost_test_render_passes()-passes==1,"equivalent attachment mip ranges share the same rendering pass");
    printf("ATTACHMENT_READONLY enabled=%d size=%dx%d color=%llu depth=%llu stencil=%llu\n",enabled,width,height,(unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]),(unsigned long long)(after[2]-before[2]));
    require(after[0]==before[0]&&after[1]-before[1]==unsigned(enabled)&&after[2]==before[2],"packed readonly depth/stencil retains one store; color writer retains STORE");

    // Occlusion-only passes have no writable color output at all. Their
    // visibility result remains observable even if every attachment is RO.
    for(bool depth_only:{true,false})for(bool fragment:{true,false}){
        NVNcounterData counter{};begin();
        if(!fragment)P(nvnCommandBufferBindProgram)(&cmd.buffer,nullptr,NVN_SHADER_STAGE_FRAGMENT_BIT);
        if(depth_only)P(nvnCommandBufferSetRenderTargets)(&cmd.buffer,0,nullptr,nullptr,&depth,&mip);
        else P(nvnChannelMaskStateSetChannelMask)(&mask,0,0,0,0,0);
        P(nvnCommandBufferResetCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED);draw(red);
        P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&counter));end();
        check_pixels(0,0,255,0xffffff,7);
        printf("ATTACHMENT_OCCLUSION enabled=%d depth_only=%d fragment=%d expected=%u actual=%llu\n",enabled,depth_only,fragment,count,(unsigned long long)counter.counter);
        require(counter.counter==count,"read-only attachment pass retains occlusion visibility");
    }
    // RO -> write -> RO must split once, retain the written depth, and reject
    // the last farther fragment. A write -> RO pass must not be downgraded.
    for(bool starts_readonly:{false,true}){
        before=stats();passes=vkhost_test_render_passes();begin();if(starts_readonly)draw(green);
        P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);draw(red);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);draw(green);end();check_pixels(255,0,0,0x400000,7);
        after=stats();require(vkhost_test_render_passes()-passes==1+unsigned(enabled&&starts_readonly),"only read-only to writable promotion splits a pass");
        require(after[3]-before[3]==unsigned(enabled&&starts_readonly),"write history is retained for the entire pass");
    }
    // Preserve earlier raster writes across separate rendering instances, not
    // just transfer clears. Sample the depth again after the read-only pass.
    for(bool partial:{false,true}){
        before=stats();begin();P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);
        if(partial)P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,width/2,height);
        draw(red);P(nvnCommandBufferBarrier)(&cmd.buffer,NVN_BARRIER_ORDER_FRAGMENTS_BIT);
        P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,width,height);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);draw(green);
        P(nvnCommandBufferBarrier)(&cmd.buffer,NVN_BARRIER_ORDER_FRAGMENTS_BIT);draw(green);end();
        for(unsigned i=0;i<count;++i){bool was_written=!partial||int(i%width)<width/2;require(pixels[i*4]==(was_written?255:0)&&pixels[i*4+1]==(was_written?0:255),"readonly passes preserve prior raster depth for later tests");require(std::abs(int(depths[i]&0xffffff)-int(was_written?0x400000:0xffffff))<=1&&(depths[i]>>24)==7,"readonly passes preserve prior partial depth writes in memory");}
        after=stats();require(after[1]-before[1]==2*unsigned(enabled),"separate read-only passes exercise NONE after earlier raster stores");
    }
    // Packed D24S8 must preserve raster-written depth when a later pass only
    // modifies a subset of stencil bits, then make that depth observable again.
    begin();P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);draw(red);P(nvnCommandBufferBarrier)(&cmd.buffer,NVN_BARRIER_ORDER_FRAGMENTS_BIT);
    P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);P(nvnDepthStencilStateSetStencilTestEnable)(&ds,1);P(nvnDepthStencilStateSetDepthFunc)(&ds,NVN_DEPTH_FUNC_ALWAYS);P(nvnDepthStencilStateSetStencilOp)(&ds,NVN_FACE_FRONT_AND_BACK,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_REPLACE);
    P(nvnCommandBufferSetStencilMask)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,1);P(nvnCommandBufferSetStencilRef)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,8);draw(green);end();check_pixels(0,255,0,0x400000,6);
    // Occlusion from a depth-only RO pass after a partial raster depth pass.
    begin();P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,width/2,height);draw(red);P(nvnCommandBufferBarrier)(&cmd.buffer,NVN_BARRIER_ORDER_FRAGMENTS_BIT);
    P(nvnCommandBufferSetRenderTargets)(&cmd.buffer,0,nullptr,nullptr,&depth,&mip);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,width,height);
    NVNcounterData partial_counter{};P(nvnCommandBufferResetCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED);draw(green);P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&partial_counter));end();require(partial_counter.counter==count/2,"depth-only readonly occlusion sees partial earlier depth writes");
    // A disabled depth test suppresses depth writes despite depthWriteEnable.
    before=stats();auto prune_before=vkhost_test_batch_work(4);begin();P(nvnDepthStencilStateSetDepthTestEnable)(&ds,0);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);draw(red);end();check_pixels(255,0,0,0xffffff,7);after=stats();auto pruned=vkhost_test_batch_work(4)-prune_before;require(pruned<=1&&after[1]-before[1]==unsigned(enabled&&!pruned),"disabled depth test preserves depth via omitted attachment or read-only store");
    // Color masks and stencil masks are independent. A zero write mask permits
    // read-only preservation even when REPLACE is selected on both faces.
    before=stats();begin();P(nvnChannelMaskStateSetChannelMask)(&mask,0,0,0,0,0);P(nvnDepthStencilStateSetStencilTestEnable)(&ds,1);P(nvnDepthStencilStateSetStencilOp)(&ds,NVN_FACE_FRONT_AND_BACK,NVN_STENCIL_OP_REPLACE,NVN_STENCIL_OP_REPLACE,NVN_STENCIL_OP_REPLACE);P(nvnCommandBufferSetStencilMask)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,0);draw(red);end();check_pixels(0,0,255,0xffffff,7);after=stats();for(unsigned i=0;i<3;++i)require(after[i]-before[i]==unsigned(enabled&&i!=2),"zero masks omit stores but preserve one packed depth/stencil store");
    // Stencil KEEP -> REPLACE -> KEEP must preserve the new stencil value and
    // must not force a depth write. Test both face operation records.
    for(auto face:{NVN_FACE_FRONT,NVN_FACE_BACK,NVN_FACE_FRONT_AND_BACK}){
        before=stats();passes=vkhost_test_render_passes();begin();P(nvnDepthStencilStateSetStencilTestEnable)(&ds,1);draw(red);P(nvnDepthStencilStateSetStencilOp)(&ds,face,NVN_STENCIL_OP_REPLACE,NVN_STENCIL_OP_REPLACE,NVN_STENCIL_OP_REPLACE);draw(red);P(nvnDepthStencilStateSetStencilOp)(&ds,NVN_FACE_FRONT_AND_BACK,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP);draw(green);end();
        // Facing depends on viewport orientation; both-face case is unambiguous.
        if(face==NVN_FACE_FRONT_AND_BACK)check_pixels(0,255,0,0xffffff,9);else for(unsigned i=0;i<count;++i)require((depths[i]&0xffffff)==0xffffff&&((depths[i]>>24)==7||(depths[i]>>24)==9),"single-face stencil preserves depth");
        after=stats();require(after[3]==before[3]&&vkhost_test_render_passes()-passes==1,"packed stencil STORE already permits later writes on either face");
    }
    // Changing color masks from no writes to partial writes must split too.
    before=stats();passes=vkhost_test_render_passes();begin();P(nvnChannelMaskStateSetChannelMask)(&mask,0,0,0,0,0);draw(green);P(nvnChannelMaskStateSetChannelMask)(&mask,0,1,0,0,0);draw(red);end();check_pixels(255,0,255,0xffffff,7);after=stats();require(after[3]-before[3]==unsigned(enabled)&&vkhost_test_render_passes()-passes==1+unsigned(enabled),"partial color writes cannot follow NONE in the same pass");
    // GPU sampling after the read-only pass must see the old raster-written
    // depth without relying on a transfer-layout transition or CPU readback.
    auto sample_controls=controls;sample_controls[1][8]=42;NVNshaderData sample_sd[2]{sd[0],{sd[1].data,sample_controls[1].data()}};
    vkhost_test_attachment_program(&sample_sd[1]);NVNprogram sample_program{};P(nvnProgramInitialize)(&sample_program,&device);P(nvnProgramSetShaders)(&sample_program,2,sample_sd);
    alignas(4096) std::array<uint8_t,4096> pool_bytes{};NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,pool_bytes.data(),pool_bytes.size());P(nvnMemoryPoolInitialize)(&pool,&pb);
    NVNtexturePool tp{};NVNsamplerPool sp{};NVNsamplerBuilder sb{};NVNsampler sampler{};P(nvnTexturePoolInitialize)(&tp,&pool,0,2);P(nvnSamplerPoolInitialize)(&sp,&pool,1024,1);P(nvnTexturePoolRegisterTexture)(&tp,1,&depth,&mip);P(nvnSamplerBuilderSetDefaults)(&sb);P(nvnSamplerInitialize)(&sampler,&sb);P(nvnSamplerPoolRegisterSampler)(&sp,0,&sampler);
    for(bool shared_attachment:{false,true}){
        before=stats();begin();P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);draw(red);P(nvnCommandBufferBarrier)(&cmd.buffer,NVN_BARRIER_ORDER_FRAGMENTS_BIT);
        P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);draw(green);P(nvnCommandBufferBarrier)(&cmd.buffer,NVN_BARRIER_ORDER_FRAGMENTS_BIT);
        P(nvnCommandBufferSetRenderTargets)(&cmd.buffer,1,targets,views,shared_attachment?&depth:nullptr,shared_attachment?&mip:nullptr);
        P(nvnCommandBufferSetTexturePool)(&cmd.buffer,&tp);P(nvnCommandBufferSetSamplerPool)(&cmd.buffer,&sp);P(nvnCommandBufferBindProgram)(&cmd.buffer,&sample_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
        P(nvnDepthStencilStateSetDepthFunc)(&ds,NVN_DEPTH_FUNC_ALWAYS);draw(green);end();
        for(unsigned i=0;i<count;++i)require(pixels[i*4]==64&&pixels[i*4+1]==64&&pixels[i*4+2]==64,"shader sampling sees preserved depth after readonly attachment stores");
        after=stats();require(after[4]-before[4]==unsigned(enabled&&shared_attachment),"sampled attachment conservatively rejects store omission");
    }
    P(nvnTexturePoolFinalize)(&tp);P(nvnSamplerPoolFinalize)(&sp);P(nvnSamplerFinalize)(&sampler);P(nvnMemoryPoolFinalize)(&pool);P(nvnProgramFinalize)(&sample_program);
    P(nvnTextureFinalize)(&color);P(nvnTextureFinalize)(&depth);P(nvnProgramFinalize)(&program);
    printf("PASS attachment stores enabled=%d: readonly, mip range, depth history, both stencil faces, masks and promotion pixels\n",enabled);
}
