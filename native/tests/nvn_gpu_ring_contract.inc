extern "C" void vkhost_test_hold_gpu(int);
extern "C" void vkhost_test_release_gpu();
extern "C" uint64_t vkhost_test_encoded_count();
extern "C" uint64_t vkhost_test_submitted_count();
static void gpu_ring_contract(NVNqueue& queue,int asynchronous){
    using namespace std::chrono_literals;
    constexpr size_t count=8;
    std::array<std::unique_ptr<Commands>,count> commands;
    std::array<NVNtexture,count> textures{};
    std::array<NVNsync,count> fences{};
    std::array<NVNcounterData,count> timestamps{};
    alignas(16) std::array<uint32_t,32> scratch{};
    NVNtextureBuilder tb{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,8,4);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    NVNcopyRegion region{0,0,0,8,4,1};
    for(size_t i=0;i<count;i++){
        P(nvnTextureInitialize)(&textures[i],&tb);P(nvnSyncInitialize)(&fences[i],&device);commands[i]=std::make_unique<Commands>();
        std::array<uint32_t,32> values;values.fill(0xff000000u|uint32_t(i+1)*0x010203u);
        auto& c=*commands[i];c.begin();
        P(nvnCommandBufferUpdateUniformBuffer)(&c.buffer,reinterpret_cast<uint64_t>(scratch.data()),sizeof(scratch),0,sizeof(values),values.data());
        P(nvnCommandBufferCopyBufferToTexture)(&c.buffer,reinterpret_cast<uint64_t>(scratch.data()),&textures[i],nullptr,&region,0);
        P(nvnCommandBufferReportCounter)(&c.buffer,NVN_COUNTER_TYPE_TIMESTAMP,reinterpret_cast<uint64_t>(&timestamps[i]));
        P(nvnCommandBufferFenceSync)(&c.buffer,&fences[i],NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    }
    vkhost_test_hold_gpu(asynchronous);
    struct Release {NVNqueue& q;~Release(){vkhost_test_release_gpu();P(nvnQueueFinish)(&q);}} release{queue};
    const auto before=vkhost_test_encoded_count(),submitted=vkhost_test_submitted_count();
    for(auto& c:commands){auto h=c->end();P(nvnQueueSubmitCommands)(&queue,1,&h);}
    const auto until=std::chrono::steady_clock::now()+2s;
    while(vkhost_test_submitted_count()-submitted<(asynchronous?3:1)&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
    // The GPU is waiting on an actual Vulkan event, rather than a mock CPU
    // queue. Three unique arenas/query pools can be pending; the fourth must
    // stop before overwriting a live slot. The synchronous negative stops at 1.
    std::this_thread::sleep_for(20ms);
    require(vkhost_test_submitted_count()-submitted==uint64_t(asynchronous?3:1),"GPU ring has bounded in-flight submissions and synchronous negative control");
    require(vkhost_test_encoded_count()-before==uint64_t(asynchronous?3:0),"translation advances while actual GPU work remains incomplete");
    for(size_t i=0;i<count;i++){
        require(P(nvnSyncWait)(&fences[i],0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"no public fence can signal before actual GPU completion");
        require(timestamps[i].timestamp==0,"query result remains unavailable until GPU completion");
    }
    vkhost_test_release_gpu();P(nvnQueueFinish)(&queue);
    for(size_t i=0;i<count;i++){
        require(P(nvnSyncWait)(&fences[i],0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED,"GPU retirement publishes command fence");
        std::array<uint32_t,36> result;result.fill(0xcccccccc);
        P(nvnTextureReadTexels)(&textures[i],nullptr,&region,result.data());
        for(size_t p=0;p<32;p++)require(result[p]==(0xff000000u|uint32_t(i+1)*0x010203u),"in-flight arena snapshots retain distinct bytes across slot reuse");
        for(size_t p=32;p<result.size();p++)require(result[p]==0xcccccccc,"in-flight readback guard");
        require(timestamps[i].timestamp!=0&&(!i||timestamps[i].timestamp>=timestamps[i-1].timestamp),"query pools retire in submission order without slot contamination");
        P(nvnTextureFinalize)(&textures[i]);P(nvnSyncFinalize)(&fences[i]);
    }
    printf("PASS real GPU hold, bounded slots, arena/query ownership, fence retirement, asynchronous=%d\n",asynchronous);
}
