// Optional device-clock profiling. Query pools belong to command buffers until
// their existing completion fence retires; profiling never adds a queue wait.
// Timestamps bracket elapsed pipeline intervals (not exclusive GPU occupancy).
constexpr uint32_t gpu_profile_capacity=2048;
bool gpu_profile_enabled=false;
uint32_t gpu_timestamp_bits=0;
struct GpuProfileRegion {uint32_t first;std::string label;};
struct GpuProfileRecording {
    VkQueryPool pool{};uint32_t next=2,open=UINT32_MAX;
    std::vector<GpuProfileRegion> regions;
};
struct GpuProfileValue {uint64_t count=0;double ns=0;};
std::map<VkCommandBuffer,GpuProfileRecording> gpu_profiles;
std::vector<VkQueryPool> gpu_profile_idle;
std::map<std::string,GpuProfileValue> gpu_profile_regions;
GpuProfileValue gpu_profile_batches;
uint64_t gpu_profile_dropped=0;
uint64_t gpu_tick_delta(uint64_t first,uint64_t last,unsigned bits){
    return (last-first)&(bits>=64?UINT64_MAX:bits?((uint64_t(1)<<bits)-1):0);
}
void gpu_profile_begin(VkCommandBuffer cmd){
    if(!gpu_profile_enabled||!gpu_timestamp_bits)return;
    GpuProfileRecording record;
    if(gpu_profile_idle.empty()){
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=gpu_profile_capacity;
        check(vkCreateQueryPool(device,&ci,nullptr,&record.pool),"profile query pool");
    }else{record.pool=gpu_profile_idle.back();gpu_profile_idle.pop_back();}
    vkCmdResetQueryPool(cmd,record.pool,0,gpu_profile_capacity);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,record.pool,0);
    gpu_profiles.emplace(cmd,std::move(record));
}
void gpu_profile_region_begin(VkCommandBuffer cmd,std::string label){
    auto found=gpu_profiles.find(cmd);if(found==gpu_profiles.end())return;auto& record=found->second;
    if(record.open!=UINT32_MAX)ot2_host_die("GPU profile region overlap");
    if(record.next+2>gpu_profile_capacity){++gpu_profile_dropped;return;}
    record.open=record.next;record.next+=2;record.regions.push_back({record.open,std::move(label)});
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,record.pool,record.open);
}
void gpu_profile_region_end(VkCommandBuffer cmd){
    auto found=gpu_profiles.find(cmd);if(found==gpu_profiles.end())return;auto& record=found->second;
    if(record.open==UINT32_MAX)return;
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,record.pool,record.open+1);record.open=UINT32_MAX;
}
void gpu_profile_end(VkCommandBuffer cmd){
    auto found=gpu_profiles.find(cmd);if(found==gpu_profiles.end())return;
    if(found->second.open!=UINT32_MAX)ot2_host_die("GPU profile unfinished region");
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,found->second.pool,1);
}
void gpu_profile_retire(VkCommandBuffer cmd){
    auto found=gpu_profiles.find(cmd);if(found==gpu_profiles.end())return;auto& record=found->second;
    std::vector<uint64_t> times(record.next);
    // Caller has already waited for this command's real GPU fence.
    check(vkGetQueryPoolResults(device,record.pool,0,record.next,times.size()*8,times.data(),8,VK_QUERY_RESULT_64_BIT),"profile results");
    auto elapsed=[&](unsigned first,unsigned last){return double(gpu_tick_delta(times[first],times[last],gpu_timestamp_bits))*timestamp_period;};
    ++gpu_profile_batches.count;gpu_profile_batches.ns+=elapsed(0,1);
    for(auto& region:record.regions){auto& total=gpu_profile_regions[region.label];++total.count;total.ns+=elapsed(region.first,region.first+1);}
    if(gpu_profile_idle.size()<4)gpu_profile_idle.push_back(record.pool);else vkDestroyQueryPool(device,record.pool,nullptr);
    gpu_profiles.erase(found);
}
void gpu_profile_report(){
    if(gpu_profile_enabled||gpu_profile_batches.count){
        ot2_host_log("GPU_PROFILE enabled=%u bits=%u batches=%llu elapsed_ms_per_60_frames=%.3f dropped=%llu",gpu_profile_enabled,gpu_timestamp_bits,(unsigned long long)gpu_profile_batches.count,gpu_profile_batches.ns/1e6,(unsigned long long)gpu_profile_dropped);
        std::vector<std::pair<std::string,GpuProfileValue>> sorted(gpu_profile_regions.begin(),gpu_profile_regions.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second.ns>b.second.ns;});
        for(size_t i=0;i<std::min<size_t>(8,sorted.size());++i){const auto& [name,v]=sorted[i];ot2_host_log("GPU_PROFILE_REGION label=%s count=%llu elapsed_ms=%.3f",name.c_str(),(unsigned long long)v.count,v.ns/1e6);}
    }
    gpu_profile_regions.clear();gpu_profile_batches={};gpu_profile_dropped=0;
    gpu_profile_enabled=std::filesystem::exists(shader_directory+"/profile-gpu");
}
