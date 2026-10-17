extern "C" void vkhost_test_compute_program(const NVNshaderData*);
extern "C" uint64_t vkhost_test_buffer_mirrors(int);
extern "C" void vkhost_test_precise_draw_dependencies(int);
static void buffer_contract(NVNqueue& queue,int enabled){
    auto before=vkhost_test_buffer_mirrors(enabled);
    std::array<uint8_t,0x880> control{};uint64_t code=0;uint32_t magic=0x98761234,size=8,stage=5;memcpy(control.data(),&magic,4);memcpy(control.data()+0x704,&size,4);memcpy(control.data()+0x714,&stage,4);
    NVNshaderData shader{reinterpret_cast<uint64_t>(&code),control.data()};vkhost_test_compute_program(&shader);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,1,&shader);
    alignas(256) std::array<uint32_t,16> data;data.fill(0xabcdef);for(unsigned i=0;i<4;i++)data[i]=i+1;std::array<uint32_t,4> copied{},replacement{100,200,300,400};const uint32_t step[]={3,5,7,11};
    Commands command;auto begin=[&]{command.begin();P(nvnCommandBufferBindProgram)(&command.buffer,&program,NVN_SHADER_STAGE_COMPUTE_BIT);};
    auto bind=[&](uint32_t* address){P(nvnCommandBufferBindStorageBuffer)(&command.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(address),16);};
    auto dispatch=[&]{P(nvnCommandBufferDispatchCompute)(&command.buffer,1,1,1);};
    auto finish=[&]{auto h=command.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);};
    begin();bind(data.data());dispatch();dispatch();P(nvnCommandBufferCopyBufferToBuffer)(&command.buffer,reinterpret_cast<uint64_t>(data.data()),reinterpret_cast<uint64_t>(copied.data()),16,0);
    P(nvnCommandBufferUpdateUniformBuffer)(&command.buffer,reinterpret_cast<uint64_t>(data.data()),16,0,16,replacement.data());dispatch();finish();
    for(unsigned i=0;i<4;i++)require(copied[i]==i+1+2*step[i]&&data[i]==replacement[i]+step[i],"GPU compute chain, ordered CPU read and CPU rewrite");
    for(unsigned i=4;i<data.size();i++)require(data[i]==0xabcdef,"GPU mirror writeback preserves neighboring guest bytes");
    for(unsigned i=0;i<6;i++)data[i]=i+1;begin();bind(data.data());dispatch();bind(data.data()+2);dispatch();finish();
    for(unsigned i=0;i<6;i++){uint32_t expected=i+1+(i<4?step[i]:0)+(i>=2?step[i-2]:0);require(data[i]==expected,"partially overlapping alias takes safe CPU synchronization fallback");}
    auto after=vkhost_test_buffer_mirrors(enabled);require(enabled?after>before:after==before,"GPU mirror mechanism and disabled negative control exercised");
    if(enabled)for(int precise:{0,1})for(int dependent:{0,1}){
        vkhost_test_precise_draw_dependencies(precise);
        alignas(256) std::array<std::array<uint32_t,64>,2> separate{};
        Commands first,second;
        auto record=[&](Commands& c,uint32_t* address){c.begin();P(nvnCommandBufferBindProgram)(&c.buffer,&program,NVN_SHADER_STAGE_COMPUTE_BIT);
            P(nvnCommandBufferBindStorageBuffer)(&c.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(address),16);
            P(nvnCommandBufferDispatchCompute)(&c.buffer,1,1,1);return c.end();};
        auto h1=record(first,separate[0].data()),h2=record(second,separate[dependent?0:1].data());
        vkhost_test_hold_gpu(1);
        auto encoded=vkhost_test_encoded_count();
        P(nvnQueueSubmitCommands)(&queue,1,&h1);P(nvnQueueSubmitCommands)(&queue,1,&h2);
        auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(precise&&!dependent?2000:100);
        while(vkhost_test_encoded_count()-encoded<2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        auto advanced=vkhost_test_encoded_count()-encoded;
        bool unpublished=true;for(auto& block:separate)for(auto word:block)unpublished&=word==0;
        vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);
        printf("PRECISE_DRAW_DEPENDENCY enabled=%d overlap=%d encoded=%llu\n",precise,dependent,(unsigned long long)advanced);
        require(unpublished,"blocked GPU storage writebacks stay invisible to guest");
        require(advanced==(precise&&!dependent?2:1),"unrelated GPU writeback must not serialize next draw; real alias still waits");
        for(unsigned b=0;b<2;b++)for(unsigned i=0;i<64;i++){
            uint32_t expected=i<4?step[i]*(dependent?(b==0?2:0):1):0;
            require(separate[b][i]==expected,"cross-submit precise dependencies preserve values and guards");
        }
    }
    vkhost_test_precise_draw_dependencies(1);
    P(nvnProgramFinalize)(&program);printf("PASS frontend GPU storage chain/read/rewrite/partial alias/guards mirror=%d reuses=%llu\n",enabled,(unsigned long long)(after-before));
}
