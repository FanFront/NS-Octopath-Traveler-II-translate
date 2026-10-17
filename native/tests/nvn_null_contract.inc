extern "C" void vkhost_test_null_program(const NVNshaderData*);
static void null_resource_contract(NVNqueue& queue){
    std::array<uint8_t,0x880> control{};uint64_t code=0;uint32_t magic=0x98761234,size=8,stage=5;memcpy(control.data(),&magic,4);memcpy(control.data()+0x704,&size,4);memcpy(control.data()+0x714,&stage,4);control[0x40]=83;
    NVNshaderData shader{reinterpret_cast<uint64_t>(&code),control.data()};vkhost_test_null_program(&shader);NVNprogram program{};P(nvnProgramInitialize)(&program,&device);P(nvnProgramSetShaders)(&program,1,&shader);
    alignas(256) std::array<float,60> output;
    alignas(4096) std::array<uint8_t,65536> descriptors{};NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};NVNtexturePool texture_pool{};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,descriptors.data(),descriptors.size());P(nvnMemoryPoolInitialize)(&pool,&pb);P(nvnTexturePoolInitialize)(&texture_pool,&pool,0,1024);
    Commands c;
    std::vector<uint64_t> handles{0,255,1024,0xfffff};if(getenv("OT2_NULL_MISSING_INSIDE_POOL"))handles={300};
    for(auto handle:handles){
        output.fill(1234);c.begin();P(nvnCommandBufferBindProgram)(&c.buffer,&program,NVN_SHADER_STAGE_COMPUTE_BIT);
        P(nvnCommandBufferSetTexturePool)(&c.buffer,&texture_pool);
        P(nvnCommandBufferBindStorageBuffer)(&c.buffer,NVN_SHADER_STAGE_COMPUTE,0,reinterpret_cast<uint64_t>(output.data()),56*sizeof(float));
        P(nvnCommandBufferBindStorageBuffer)(&c.buffer,NVN_SHADER_STAGE_COMPUTE,1,0,0);
        P(nvnCommandBufferBindUniformBuffer)(&c.buffer,NVN_SHADER_STAGE_COMPUTE,1,0,0);
        P(nvnCommandBufferBindTexture)(&c.buffer,NVN_SHADER_STAGE_COMPUTE,0,handle);
        P(nvnCommandBufferDispatchCompute)(&c.buffer,1,1,1);P(nvnCommandBufferDispatchCompute)(&c.buffer,1,1,1);
        auto h=c.end();P(nvnQueueSubmitCommands)(&queue,1,&h);P(nvnQueueFinish)(&queue);
        for(unsigned i=0;i<56;i++)require(output[i]==0,"unbound image/buffer reads and atomics return zero; writes discarded");
        for(unsigned i=56;i<output.size();i++)require(output[i]==1234,"null resource writes leave unrelated output guards intact");
    }
    P(nvnTexturePoolFinalize)(&texture_pool);P(nvnMemoryPoolFinalize)(&pool);P(nvnProgramFinalize)(&program);puts("PASS null resources: zero/reserved/out-of-pool/sentinel, 2D/3D/cube/array/MS/shadow/texel, image/buffer stores and atomics");
}
