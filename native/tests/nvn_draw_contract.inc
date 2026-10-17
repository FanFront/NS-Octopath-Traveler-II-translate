extern "C" void vkhost_test_state_program(const NVNshaderData*);
extern "C" uint64_t vkhost_test_binding_cache(int);
extern "C" uint64_t vkhost_test_snapshot_cache(int);
extern "C" uint64_t vkhost_test_gpu_submissions();
extern "C" uint64_t vkhost_test_render_passes();
extern "C" void vkhost_test_query_batching(int);
extern "C" void vkhost_test_hold_gpu(int);
extern "C" void vkhost_test_release_gpu();
extern "C" uint64_t vkhost_test_encoded_count();
extern "C" uint64_t vkhost_test_submitted_count();
extern "C" uint64_t vkhost_test_draw_reuse(int);
extern "C" uint64_t vkhost_test_prepare_reuse(int);
extern "C" uint64_t vkhost_test_resource_preparation();
extern "C" uint64_t vkhost_test_dynamic_descriptors(int);
extern "C" uint64_t vkhost_test_descriptor_contents(int);
extern "C" uint64_t vkhost_test_layout_inputs(int);
extern "C" uint64_t vkhost_test_dependency_index(uintptr_t,size_t,int,int*);
extern "C" uint64_t vkhost_test_dependency_lookup(uintptr_t,size_t,int,int*);
extern "C" uint64_t vkhost_test_dependency_union(uintptr_t,size_t,int*);
extern "C" uint64_t vkhost_test_dependency_memo(uintptr_t,size_t,int*);
extern "C" uint64_t vkhost_test_command_index(int);
extern "C" void vkhost_test_reflected_preflight(int);
static void draw_contract(NVNqueue& queue,int reuse){
    vkhost_test_query_batching(reuse);
    auto reuse_before=vkhost_test_binding_cache(reuse);
    auto snapshot_before=vkhost_test_snapshot_cache(reuse);
    auto objects_before=vkhost_test_draw_reuse(reuse);
    auto prepare_before=vkhost_test_prepare_reuse(reuse);
    auto resource_prepare_before=vkhost_test_resource_preparation();
    auto layout_before=vkhost_test_layout_inputs(reuse);
    std::array<std::array<uint8_t,0x880>,2> control{};uint64_t code[2]{};NVNshaderData sd[2]{};
    for(unsigned i=0;i<2;i++){uint32_t magic=0x98761234,size=8;memcpy(control[i].data(),&magic,4);memcpy(control[i].data()+0x704,&size,4);memcpy(control[i].data()+0x714,&i,4);sd[i]={reinterpret_cast<uint64_t>(&code[i]),control[i].data()};}
    vkhost_test_state_program(sd);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,2,sd);
    NVNtextureBuilder tb{};NVNtexture color{},depth{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,16,8);P(nvnTextureBuilderSetLevels)(&tb,2);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);P(nvnTextureInitialize)(&color,&tb);
    P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH24_STENCIL8);P(nvnTextureInitialize)(&depth,&tb);
    NVNtextureView mip{},srgb{};P(nvnTextureViewSetDefaults)(&mip);P(nvnTextureViewSetLevels)(&mip,1,1);srgb=mip;P(nvnTextureViewSetFormat)(&srgb,NVN_FORMAT_RGBA8_SRGB);
    NVNpolygonState polygon{};P(nvnPolygonStateSetDefaults)(&polygon);P(nvnPolygonStateSetCullFace)(&polygon,NVN_FACE_NONE);
    NVNcolorState cs{};NVNblendState blend{};NVNchannelMaskState mask{};NVNdepthStencilState ds{};
    P(nvnColorStateSetDefaults)(&cs);P(nvnBlendStateSetDefaults)(&blend);P(nvnChannelMaskStateSetDefaults)(&mask);P(nvnDepthStencilStateSetDefaults)(&ds);
    struct alignas(16) Values{float color[4];float params[4];};
    Values half{{.5f,.5f,.5f,1},{.5f}},red{{1,0,0,.5f},{.5f}},green{{0,1,0,1},{.75f}},near{{0,1,0,1},{.25f}};
    NVNtexture* targets[]={&color};const NVNtextureView* views[]={&mip};NVNcopyRegion region{0,0,0,8,4,1};std::array<uint8_t,144> pixels{};
    Commands cmd;
    auto begin=[&](bool with_depth=false){cmd.begin();P(nvnCommandBufferSetRenderTargets)(&cmd.buffer,1,targets,views,with_depth?&depth:nullptr,with_depth?&mip:nullptr);
        P(nvnCommandBufferBindProgram)(&cmd.buffer,&program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);P(nvnCommandBufferBindPolygonState)(&cmd.buffer,&polygon);
        P(nvnCommandBufferBindColorState)(&cmd.buffer,&cs);P(nvnCommandBufferBindBlendState)(&cmd.buffer,&blend);P(nvnCommandBufferBindChannelMaskState)(&cmd.buffer,&mask);P(nvnCommandBufferBindDepthStencilState)(&cmd.buffer,&ds);
        P(nvnCommandBufferSetViewport)(&cmd.buffer,0,0,8,4);P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,8,4);float black[4]{};P(nvnCommandBufferClearTexture)(&cmd.buffer,&color,&mip,&region,black,15);};
    auto draw=[&](Values& value){P(nvnCommandBufferBindUniformBuffer)(&cmd.buffer,NVN_SHADER_STAGE_VERTEX,0,reinterpret_cast<uint64_t>(&value),sizeof(value));P(nvnCommandBufferBindUniformBuffer)(&cmd.buffer,NVN_SHADER_STAGE_FRAGMENT,0,reinterpret_cast<uint64_t>(&value),sizeof(value));P(nvnCommandBufferDrawArrays)(&cmd.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);};
    auto end=[&]{pixels.fill(0xcc);P(nvnCommandBufferCopyTextureToBuffer)(&cmd.buffer,&color,&mip,&region,reinterpret_cast<uint64_t>(pixels.data()),0);auto h=cmd.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);for(unsigned i=128;i<pixels.size();i++)require(pixels[i]==0xcc,"draw preserves mip readback guards");};
    for(bool s:{false,true}){views[0]=s?&srgb:&mip;begin();draw(half);end();for(unsigned i=0;i<32;i++)require(abs(int(pixels[i*4])-(s?188:128))<=1&&pixels[i*4+3]==255,"draw pipeline follows attachment view format");}
    views[0]=&mip;P(nvnColorStateSetBlendEnable)(&cs,0,1);P(nvnBlendStateSetBlendFunc)(&blend,NVN_BLEND_FUNC_SRC_ALPHA,NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO);
    begin();P(nvnCommandBufferSetScissor)(&cmd.buffer,2,1,3,2);draw(red);end();
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<8;x++){auto i=(y*8+x)*4;bool inside=x>=2&&x<5&&y>=1&&y<3;require(inside?pixels[i]>=127&&pixels[i]<=128:pixels[i]==0,"blend and scissor pixel extent");}
    P(nvnColorStateSetBlendEnable)(&cs,0,0);P(nvnDepthStencilStateSetDepthTestEnable)(&ds,1);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,1);P(nvnDepthStencilStateSetDepthFunc)(&ds,NVN_DEPTH_FUNC_LESS);
    begin(true);P(nvnCommandBufferClearDepthStencil)(&cmd.buffer,1,1,0,255);draw(red);draw(green);end();for(unsigned i=0;i<32;i++)require(pixels[i*4]==255&&pixels[i*4+1]==0,"farther depth fails after an earlier depth write");
    begin(true);P(nvnCommandBufferClearDepthStencil)(&cmd.buffer,1,1,0,255);draw(red);draw(near);end();for(unsigned i=0;i<32;i++)require(pixels[i*4]==0&&pixels[i*4+1]==255,"nearer depth passes");
    P(nvnDepthStencilStateSetDepthTestEnable)(&ds,0);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);P(nvnDepthStencilStateSetStencilTestEnable)(&ds,1);P(nvnDepthStencilStateSetStencilFunc)(&ds,NVN_FACE_FRONT_AND_BACK,NVN_STENCIL_FUNC_EQUAL);
    for(int ref:{6,7}){begin(true);P(nvnCommandBufferClearDepthStencil)(&cmd.buffer,1,1,7,255);P(nvnCommandBufferSetStencilRef)(&cmd.buffer,NVN_FACE_FRONT_AND_BACK,ref);draw(green);end();for(unsigned i=0;i<32;i++)require(pixels[i*4+1]==(ref==7?255:0),"stencil equality and failing negative control");}
    P(nvnDepthStencilStateSetDefaults)(&ds);
    // Repeated bindings may reuse an immutable descriptor set. A subsequent
    // inline update of the same address must create a different input snapshot.
    Values changing=red;begin();P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,4,4);draw(changing);draw(changing);
    P(nvnCommandBufferUpdateUniformBuffer)(&cmd.buffer,reinterpret_cast<uint64_t>(&changing),sizeof(changing),0,sizeof(green),&green);
    P(nvnCommandBufferSetScissor)(&cmd.buffer,4,0,4,4);draw(changing);end();
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<8;x++){auto i=(y*8+x)*4;require(pixels[i]==(x<4?255:0)&&pixels[i+1]==(x<4?0:255),"same address update retains old and new GPU input versions");}
    // Change only the fragment bank. The vertex resource group must remain
    // usable while the full descriptor preparation changes, with actual pixel
    // output checking both the retained and replaced groups.
    begin();P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,4,4);draw(red);
    // An unrelated bank change invalidates the whole-set snapshot but must
    // not rebuild the nonempty constant-buffer group. Repeat once after the
    // first draw has materialized the attachment clear and started its slot.
    for(uint64_t handle:{0x123ULL,0x456ULL}){
        P(nvnCommandBufferBindTexture)(&cmd.buffer,NVN_SHADER_STAGE_FRAGMENT,127,handle);
        P(nvnCommandBufferDrawArrays)(&cmd.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    }
    P(nvnCommandBufferSetScissor)(&cmd.buffer,4,0,4,4);
    P(nvnCommandBufferBindUniformBuffer)(&cmd.buffer,NVN_SHADER_STAGE_FRAGMENT,0,reinterpret_cast<uint64_t>(&green),sizeof(green));
    P(nvnCommandBufferDrawArrays)(&cmd.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);end();
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<8;x++){auto i=(y*8+x)*4;require(pixels[i]==(x<4?255:0)&&pixels[i+1]==(x<4?0:255),"partial stage bank change retains vertex group and replaces fragment group");}
    // A full clear is the next observable use of the color subresource.
    // Its output must survive even when the preceding draw store is discarded.
    begin();
    require(cmd.used+4096<=cmd.memory.size(),"contiguous store-plan command capacity");
    P(nvnCommandBufferAddCommandMemory)(&cmd.buffer,&cmd.pool,cmd.used,4096);cmd.used+=4096;
    draw(red);float clear_blue[4]={0,0,1,1};
    P(nvnCommandBufferClearColor)(&cmd.buffer,0,clear_blue,15);end();
    for(unsigned i=0;i<32;i++)require(pixels[i*4]==0&&pixels[i*4+1]==0&&pixels[i*4+2]==255&&pixels[i*4+3]==255,"next full clear replaces an unobserved draw store");
    for(int expanded:{0,1}){
        Commands clear;clear.begin();P(nvnCommandBufferSetRenderTargets)(&clear.buffer,1,targets,views,nullptr,nullptr);
        P(nvnCommandBufferSetScissor)(&clear.buffer,0,0,8,4);P(nvnCommandBufferClearColor)(&clear.buffer,0,clear_blue,15);auto nested=clear.end();
        auto stores=vkhost_test_command_index(expanded);begin();draw(red);P(nvnCommandBufferCallCommands)(&cmd.buffer,1,&nested);end();
        for(unsigned i=0;i<32;i++)require(pixels[i*4]==0&&pixels[i*4+2]==255,"expanded call clear preserves final pixels");
        if(!getenv("OT2_BATCH_BASELINE")&&!getenv("OT2_SOFTWARE_SAMPLES_ONLY"))require(vkhost_test_command_index(expanded)-stores==unsigned(expanded),"only indexed lookahead proves dead store across CALL and target bind");
        // A transfer observation before the clear prohibits discarding red.
        std::array<uint8_t,128> observed{};begin();draw(red);
        P(nvnCommandBufferCopyTextureToBuffer)(&cmd.buffer,&color,&mip,&region,reinterpret_cast<uint64_t>(observed.data()),0);
        P(nvnCommandBufferCallCommands)(&cmd.buffer,1,&nested);end();
        for(unsigned i=0;i<32;i++)require(observed[i*4]==255&&observed[i*4+2]==0&&pixels[i*4+2]==255,"transfer observation stops expanded store elision");
    }
    vkhost_test_command_index(!getenv("OT2_BATCH_BASELINE"));
    // One logical counter interval alternates native depth-tested queries and
    // the software color-only path. Readback must skip unused physical slots.
    std::array<NVNcounterData,3> mixed{};begin(true);
    P(nvnCommandBufferClearDepthStencil)(&cmd.buffer,1,1,0,255);
    P(nvnCommandBufferResetCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED);
    for(unsigned i=0;i<3;i++){
        P(nvnDepthStencilStateSetDepthTestEnable)(&ds,i!=1);P(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);P(nvnDepthStencilStateSetDepthFunc)(&ds,NVN_DEPTH_FUNC_ALWAYS);
        P(nvnCommandBufferBindDepthStencilState)(&cmd.buffer,&ds);draw(green);
        P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&mixed[i]));
    }
    end();for(unsigned i=0;i<3;i++)require(mixed[i].counter==32*(i+1),"mixed native/software query slots retain cumulative counts");
    P(nvnDepthStencilStateSetDefaults)(&ds);
    // Independent cumulative and reset ranges in one command stream. Empty
    // ranges, reports made before later resets, and cross-submit accumulation
    // must retain their own meaning without a CPU fence for every reset.
    std::array<NVNcounterData,28> counters{};auto submits=vkhost_test_gpu_submissions(),passes=vkhost_test_render_passes();begin();
    for(unsigned i=0;i<12;i++){
        P(nvnCommandBufferResetCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED);
        P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&counters[2*i]));
        P(nvnCommandBufferSetScissor)(&cmd.buffer,0,0,1+i%8,1);draw(green);
        P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&counters[2*i+1]));
    }
    end();auto actual_submits=vkhost_test_gpu_submissions()-submits,actual_passes=vkhost_test_render_passes()-passes;
    for(unsigned i=0;i<12;i++)require(counters[2*i].counter==0&&counters[2*i+1].counter==1+i%8&&counters[2*i+1].timestamp>=counters[2*i].timestamp,"occlusion report snapshots reset interval and excludes previous intervals");
    begin();P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&counters[24]));draw(green);
    P(nvnCommandBufferReportCounter)(&cmd.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&counters[25]));end();
    require(counters[24].counter==4&&counters[25].counter==36,"occlusion accumulation survives real GPU submission boundaries");
    printf("OCCLUSION_RESET submits=%llu passes=%llu\n",(unsigned long long)actual_submits,(unsigned long long)actual_passes);
    require(reuse?actual_submits<5:actual_submits>=12,"batched occlusion resets and synchronous negative control have distinct submission counts");
    require(reuse&&!getenv("OT2_SOFTWARE_SAMPLES_ONLY")?actual_passes==1:actual_passes>=12,"native counter-only commands preserve a pass; software counter writes retain explicit boundaries");
    if(reuse){
        // An in-flight GPU write bound to an unused constant bank must not
        // stall an unrelated draw. A real held fence makes the old all-banks
        // preflight a negative control, while the pixels still check used UBOs.
        for(int reflected:{0,1}){
            vkhost_test_reflected_preflight(reflected);
            begin();draw(green);end();
            NVNcounterData unrelated{};Commands writer,reader;
            writer.begin();P(nvnCommandBufferReportCounter)(&writer.buffer,NVN_COUNTER_TYPE_TIMESTAMP,reinterpret_cast<uint64_t>(&unrelated));auto wh=writer.end();
            reader.begin();
            for(auto stage:{NVN_SHADER_STAGE_VERTEX,NVN_SHADER_STAGE_FRAGMENT})P(nvnCommandBufferBindUniformBuffer)(&reader.buffer,stage,15,reinterpret_cast<uint64_t>(&unrelated),sizeof(unrelated));
            P(nvnCommandBufferDrawArrays)(&reader.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            for(auto stage:{NVN_SHADER_STAGE_VERTEX,NVN_SHADER_STAGE_FRAGMENT})P(nvnCommandBufferBindUniformBuffer)(&reader.buffer,stage,15,0,0);
            auto rh=reader.end();vkhost_test_hold_gpu(1);
            const auto before=vkhost_test_encoded_count();
            P(nvnQueueSubmitCommands)(&queue,1,&wh);P(nvnQueueSubmitCommands)(&queue,1,&rh);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(reflected?2000:100);
            while(vkhost_test_encoded_count()-before<2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
            const auto encoded=vkhost_test_encoded_count()-before;const bool unpublished=!unrelated.timestamp;
            vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);
            require(encoded==uint64_t(reflected?2:1)&&unpublished,"only actually referenced constant banks constrain CPU translation");
            pixels.fill(0xcc);P(nvnTextureReadTexels)(&color,&mip,&region,pixels.data());
            for(unsigned i=0;i<32;++i)require(pixels[i*4]==0&&pixels[i*4+1]==255,"reflected preflight preserves used uniform pixels");
            for(unsigned i=128;i<pixels.size();++i)require(pixels[i]==0xcc,"reflected preflight readback guards");
        }
        vkhost_test_reflected_preflight(1);
        puts("PASS reflected preflight: unused bank avoids held GPU dependency, used UBO pixels preserved");
        // Multiple reset epochs must be evaluated by GPU retirement order,
        // not by the translator's newest reset or a prematurely sampled base.
        std::array<std::unique_ptr<Commands>,3> pending;
        std::array<NVNcounterData,6> results{},copied{};
        for(unsigned i=0;i<pending.size();i++){
            pending[i]=std::make_unique<Commands>();auto& c=*pending[i];c.begin();
            if(i!=1)P(nvnCommandBufferResetCounter)(&c.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED);
            P(nvnCommandBufferReportCounter)(&c.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&results[2*i]));
            P(nvnCommandBufferSetScissor)(&c.buffer,0,0,i+1,1);
            P(nvnCommandBufferBindUniformBuffer)(&c.buffer,NVN_SHADER_STAGE_VERTEX,0,reinterpret_cast<uint64_t>(&green),sizeof(green));
            P(nvnCommandBufferBindUniformBuffer)(&c.buffer,NVN_SHADER_STAGE_FRAGMENT,0,reinterpret_cast<uint64_t>(&green),sizeof(green));
            P(nvnCommandBufferDrawArrays)(&c.buffer,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            P(nvnCommandBufferReportCounter)(&c.buffer,NVN_COUNTER_TYPE_SAMPLES_PASSED,reinterpret_cast<uint64_t>(&results[2*i+1]));
        }
        int memo_result=0;
        for(auto& result:results){
            vkhost_test_dependency_union(uintptr_t(&result),sizeof(result),&memo_result);require(!memo_result,"union negative before publication");
            auto first=vkhost_test_dependency_memo(uintptr_t(&result),sizeof(result),&memo_result);require(!memo_result,"no writer before publication");
            auto again=vkhost_test_dependency_memo(uintptr_t(&result),sizeof(result),&memo_result);require(!memo_result&&again>first,"identical negative range is memoized");
        }
        vkhost_test_hold_gpu(1);
        struct Release{NVNqueue& queue;~Release(){vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);}} release{queue};
        auto before=vkhost_test_encoded_count();
        for(auto& c:pending){auto h=c->end();P(nvnQueueSubmitCommands)(&queue,1,&h);}
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(vkhost_test_encoded_count()-before<3&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
        require(vkhost_test_encoded_count()-before==3,"occlusion epochs remain in flight across three batches");
        for(auto& result:results){vkhost_test_dependency_memo(uintptr_t(&result),sizeof(result),&memo_result);require(memo_result,"new held GPU writer invalidates previously memoized negative range");}
        for(auto& result:results){vkhost_test_dependency_union(uintptr_t(&result),sizeof(result),&memo_result);require(memo_result,"union includes newly published held GPU counters");}
        for(auto& c:results)require(c.counter==0&&c.timestamp==0,"in-flight counters do not become CPU-visible early");
        // Negative dependency checks dominate the real command replay. Keep
        // the GPU genuinely blocked while verifying both positive dependencies
        // and the bounded fast rejection for a disjoint allocation.
        std::vector<uint8_t> unrelated(4096);int dependency=0;
        auto skips=vkhost_test_dependency_union(uintptr_t(unrelated.data()),unrelated.size(),&dependency);require(!dependency,"union disjoint allocation");
        require(vkhost_test_dependency_union(uintptr_t(unrelated.data()),unrelated.size(),&dependency)>skips&&!dependency,"union fast negative path exercised");
        for(int bounded:{0,1}){
            auto start=vkhost_test_dependency_lookup(uintptr_t(unrelated.data()),unrelated.size(),bounded,&dependency);require(!dependency,"disjoint allocation has no pending GPU writer");
            uint64_t finish=start;
            for(unsigned i=0;i<100;++i){finish=vkhost_test_dependency_lookup(uintptr_t(unrelated.data()),unrelated.size(),bounded,&dependency);require(!dependency,"disjoint range remains independent");}
            require(bounded?finish==start:finish>start,"bounded rejection avoids scanning unrelated GPU writebacks");
            vkhost_test_dependency_lookup(uintptr_t(&results[1]),sizeof(results[1]),bounded,&dependency);require(dependency,"real pending counter write remains a dependency");
        }
        for(int indexed:{0,1}){
            auto start=vkhost_test_dependency_index(uintptr_t(unrelated.data()),unrelated.size(),indexed,&dependency);require(!dependency,"indexed disjoint range has no GPU dependency");
            auto end=start;
            for(auto& result:results){end=vkhost_test_dependency_index(uintptr_t(&result),sizeof(result),indexed,&dependency);require(dependency,"indexed CPU access preserves every real pending GPU write");}
            require(indexed?end>start:end==start,"exact range index exercised with disabled negative control");
        }
        Commands read;read.begin();P(nvnCommandBufferCopyBufferToBuffer)(&read.buffer,reinterpret_cast<uint64_t>(results.data()),reinterpret_cast<uint64_t>(copied.data()),sizeof(results),0);auto h=read.end();P(nvnQueueSubmitCommands)(&queue,1,&h);
        vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);
        const uint64_t expected[]={0,1,1,3,0,3};
        for(unsigned i=0;i<6;i++)require(results[i].counter==expected[i]&&copied[i].counter==expected[i]&&copied[i].timestamp==results[i].timestamp,"in-flight counter epoch accumulation and ordered CPU readback");
        puts("PASS three GPU batches retain reset epochs and CPU query dependencies");
    }
    if(reuse){
        // One large guest submit must fill all three slots before waiting.
        // The 128-operation capacity boundary is not a CPU-visible dependency.
        begin();for(unsigned i=0;i<400;i++)draw(green);
        auto h=cmd.end();auto submitted=vkhost_test_submitted_count();
        vkhost_test_hold_gpu(1);
        struct ReleaseCapacity{NVNqueue& q;~ReleaseCapacity(){vkhost_test_release_gpu();P(nvnQueueFinish)(&q);}} release{queue};
        P(nvnQueueSubmitCommands)(&queue,1,&h);
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(vkhost_test_submitted_count()-submitted<3&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
        require(vkhost_test_submitted_count()-submitted==3,"capacity flush keeps three GPU slots in flight inside one guest submit");
        vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);
        pixels.fill(0xcc);P(nvnTextureReadTexels)(&color,&mip,&region,pixels.data());
        for(unsigned i=0;i<32;i++)require(pixels[i*4]==0&&pixels[i*4+1]==255,"capacity flush preserves descriptor and upload lifetimes");
        for(unsigned i=128;i<pixels.size();i++)require(pixels[i]==0xcc,"capacity flush readback guard");
        puts("PASS capacity flush: three pending batches, 400 draws and fence-owned resources");
    }
    if(getenv("OT2_SOFTWARE_SAMPLES_ONLY")){
        P(nvnProgramFinalize)(&program);P(nvnTextureFinalize)(&color);P(nvnTextureFinalize)(&depth);
        puts("PASS software/native sample counts, resets, mixed slots, GPU held epochs and capacity lifetimes");return;
    }
    auto reuse_after=vkhost_test_binding_cache(reuse);require(reuse?reuse_after>reuse_before:reuse_after==reuse_before,"binding cache exercised with disabled negative control");
    auto snapshot_after=vkhost_test_snapshot_cache(reuse);require(reuse?snapshot_after>snapshot_before:snapshot_after==snapshot_before,"flat snapshot cache and tree negative control preserve GPU input versions");
    auto objects_after=vkhost_test_draw_reuse(reuse);require(reuse?objects_after>objects_before:objects_after==objects_before,"draw object reuse exercised with disabled negative control");
    auto prepare_after=vkhost_test_prepare_reuse(reuse);require(reuse?prepare_after>prepare_before:prepare_after==prepare_before,"pipeline preparation reuse exercised with disabled negative control");
    auto layout_after=vkhost_test_layout_inputs(reuse);require(reuse?layout_after>layout_before:layout_after==layout_before,"layout input cache exercised with disabled negative control");
    auto resource_prepare_after=vkhost_test_resource_preparation();require(reuse?resource_prepare_after>resource_prepare_before:resource_prepare_after==resource_prepare_before,"whole resource preparation skipped with disabled negative control");
    if(reuse)for(int dynamic:{0,1}){
        vkhost_test_descriptor_contents(0); // Isolate dynamic offsets from retired-content reuse.
        std::array<Values,32> distinct{};
        auto updates=vkhost_test_dynamic_descriptors(dynamic);begin();
        for(unsigned i=0;i<distinct.size();++i){
            distinct[i]={{float(i+1)/32,0,0,1},{.5f}};
            P(nvnCommandBufferSetScissor)(&cmd.buffer,i%8,i/8,1,1);draw(distinct[i]);
        }
        end();
        for(unsigned i=0;i<32;++i)require(abs(int(pixels[4*i])-int(std::lround(255.0*(i+1)/32)))<=1&&pixels[4*i+3]==255,"dynamic offsets retain every distinct uniform snapshot");
        auto count=vkhost_test_dynamic_descriptors(dynamic)-updates;
        printf("DYNAMIC_DESCRIPTORS enabled=%d updates=%llu draws=32\n",dynamic,(unsigned long long)count);
        require(dynamic?count<=2:count>=32,"changing uniform offsets reuses immutable sets; static negative control updates each draw");
    }
    P(nvnProgramFinalize)(&program);P(nvnTextureFinalize)(&color);P(nvnTextureFinalize)(&depth);
    if(reuse)for(int retain:{0,1}){
        vkhost_test_descriptor_contents(retain);
        auto before=vkhost_test_dynamic_descriptors(0);
        for(unsigned round=0;round<6;++round){
            Values value{{round&1?1.f:0.f,round&1?0.f:1.f,0,1},{.5f}};
            begin();draw(value);end();
            for(unsigned i=0;i<32;++i)require(pixels[4*i]==(round&1?255:0)&&pixels[4*i+1]==(round&1?0:255),"retired descriptor contents use fresh arena bytes on each recording");
        }
        auto updates=vkhost_test_dynamic_descriptors(0)-before;
        printf("DESCRIPTOR_CONTENTS enabled=%d rounds=6 updates=%llu\n",retain,(unsigned long long)updates);
        require(retain?updates<=2:updates>=6,"unchanged retired descriptors avoid updates across recordings");
    }
    printf("PASS frontend draw: views/mips/blend/scissor/depth/stencil/update versions, binding cache=%d hits=%llu\n",reuse,(unsigned long long)(reuse_after-reuse_before));
}
