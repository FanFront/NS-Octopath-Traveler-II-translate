extern "C" void vkhost_test_uniform_range_program(const NVNshaderData*);
extern "C" uint64_t vkhost_test_compact_uniforms(int);
extern "C" int vkhost_test_robust_uniform_ranges();
static void uniform_range_contract(NVNqueue& queue){
    std::array<uint8_t,0x880> control{};uint64_t code=0;uint32_t magic=0x98761234,size=8,stage=5;
    memcpy(control.data(),&magic,4);memcpy(control.data()+0x704,&size,4);memcpy(control.data()+0x714,&stage,4);control[0x40]=97;
    NVNshaderData shader{reinterpret_cast<uint64_t>(&code),control.data()};vkhost_test_uniform_range_program(&shader);
    NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,1,&shader);
    alignas(256) std::array<uint8_t,65536> source{};
    alignas(256) std::array<uint8_t,65552> target{};
    Commands commands;
    for(int compact:{0,1}){
        auto before=vkhost_test_compact_uniforms(compact);
        for(unsigned bytes:{65536u,65535u,4095u,257u,255u,31u,13u,1u,0u}){
            for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*37+bytes+1);
            target.fill(0xcc);commands.begin();
            P(nvnCommandBufferBindProgram)(&commands.buffer,&program,NVN_SHADER_STAGE_COMPUTE_BIT);
            P(nvnCommandBufferBindUniformBuffer)(&commands.buffer,NVN_SHADER_STAGE_COMPUTE,0,bytes?reinterpret_cast<uint64_t>(source.data()):0,bytes);
            P(nvnCommandBufferBindStorageBuffer)(&commands.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(target.data()),65536);
            P(nvnCommandBufferDispatchCompute)(&commands.buffer,64,1,1);
            auto h=commands.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
            for(size_t i=0;i<65536;++i)require(target[i]==(i<bytes?source[i]:0),"uniform range preserves every byte and zeros the full out-of-bounds tail");
            for(size_t i=65536;i<target.size();++i)require(target[i]==0xcc,"uniform read probe preserves output guards");
        }
        auto allocated=vkhost_test_compact_uniforms(compact)-before;
        printf("UNIFORM_RANGE compact=%d uploaded=%llu\n",compact,(unsigned long long)allocated);
        require(compact&&vkhost_test_robust_uniform_ranges()?allocated<180000:allocated>=9*65536,"compact uploads require robust bounds; unsupported devices retain complete zero padding");
    }
    P(nvnProgramFinalize)(&program);
    puts("PASS compact uniform ranges: complete 64 KiB GPU reads, odd byte tails, shrinking same address, null input and guards");
}
