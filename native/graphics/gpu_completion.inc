// Included after draw.inc. Vulkan queue submissions remain ordered, while CPU
// translation can fill another slot. All mutable bookkeeping uses state_mutex;
// the completion worker waits for fences without holding that mutex.
#include "cached_readback.inc"
struct GpuSlot {
    VkQueryPool occlusion{},timestamps{};
    std::vector<UploadArena> arenas;
    bool busy=false;
    ReadbackBuffer readback;
};
struct GpuBatch {
    GpuSlot* slot{};
    VkCommandBuffer command{};
    VkFence fence{};
    uint32_t queries{};
    std::vector<PendingCounter> counters;
    std::vector<SampleInterval> intervals;
    std::vector<SoftwareSampleSlot> software_samples;
    std::map<uintptr_t,BufferMirror> mirrors;
    WriteRanges write_ranges;
    std::vector<std::function<void()>> retire;
    bool retired=false;
    bool staged_readback=false;
    uintptr_t write_first=UINTPTR_MAX,write_last=0;
    bool has_writes=false;
    void include_write(uintptr_t address,size_t bytes){
        if(!bytes)return;has_writes=true;
        if(bytes-1>UINTPTR_MAX-address){write_first=0;write_last=UINTPTR_MAX;return;}
        write_first=std::min(write_first,address);write_last=std::max(write_last,address+bytes-1);
    }
    bool may_write(uintptr_t address,size_t bytes)const{
        if(!has_writes||!bytes)return false;
        if(bytes-1>UINTPTR_MAX-address)return true;
        return address<=write_last&&(address>=write_first||write_first-address<bytes);
    }
    ~GpuBatch(){if(fence)vkDestroyFence(device,fence,nullptr);}
};
constexpr size_t gpu_ring_size=3;
std::array<GpuSlot,gpu_ring_size> gpu_slots;
void trim_readbacks(){for(auto& slot:gpu_slots)if(!slot.busy&&slot.readback.size>std::min<size_t>(cache_budget.resource_bytes/4,32*1024*1024))slot.readback.clear();}
GpuSlot* recording_slot=nullptr;
std::deque<std::shared_ptr<GpuBatch>> gpu_inflight;
std::shared_ptr<GpuBatch> last_gpu_batch;
bool asynchronous_gpu=true;
uint64_t gpu_ring_reuses=0,gpu_inflight_peak=0;
bool dependency_bounds_enabled=true;
uint64_t dependency_range_checks=0,dependency_bounds_skips=0;
bool dependency_memo_enabled=true;
DirectCache<2,bool> previous_read_memo;
uint64_t dependency_memo_hits=0,dependency_memo_misses=0;
bool dependency_union_enabled=true;
WriteRanges inflight_write_union;
uint64_t dependency_union_skips=0,dependency_union_checks=0;

void retire_gpu_batch(const std::shared_ptr<GpuBatch>& batch){
    if(batch->retired)return;
    FrameTraceScope trace("OT2 retire GPU batch");
    {FrameTraceScope wait_trace("OT2 ring fence wait");PerfScope timing(perf.wait_ns);check(vkWaitForFences(device,1,&batch->fence,VK_TRUE,UINT64_MAX),"GPU ring completion");}
    gpu_profile_retire(batch->command);retire_legacy_rendering(batch->command);
    std::vector<uint64_t> prefix(batch->queries+1);
    if(batch->queries){
        size_t software=0;
        for(uint32_t first=0;first<batch->queries;){
            if(software<batch->software_samples.size()&&batch->software_samples[software].index==first){
                memcpy(&prefix[first+1],batch->software_samples[software++].buffer.mapped,8);++first;
            }else{
                const uint32_t end=software<batch->software_samples.size()?batch->software_samples[software].index:batch->queries;
                check(vkGetQueryPoolResults(device,batch->slot->occlusion,first,end-first,(end-first)*8,prefix.data()+first+1,8,VK_QUERY_RESULT_64_BIT),"GPU ring occlusion results");first=end;
            }
        }
        for(size_t i=1;i<prefix.size();++i)prefix[i]+=prefix[i-1];
    }
    if(!batch->counters.empty()){
        std::vector<uint64_t> timestamps(batch->counters.size());
        check(vkGetQueryPoolResults(device,batch->slot->timestamps,0,timestamps.size(),timestamps.size()*8,timestamps.data(),8,VK_QUERY_RESULT_64_BIT),"GPU ring timestamps");
        // Earlier batches have retired in order. An epoch's accumulated base
        // therefore excludes this batch, even if translation has reset it.
        for(size_t i=0;i<timestamps.size();++i){auto& c=batch->counters[i];auto* out=reinterpret_cast<NVNcounterData*>(c.address);
            out->timestamp=uint64_t(double(timestamps[i])*timestamp_period);
            out->counter=c.type==NVN_COUNTER_TYPE_SAMPLES_PASSED?c.epoch->samples+prefix.at(c.sample_end)-prefix.at(c.sample_begin):out->timestamp;
        }
    }
    for(auto& interval:batch->intervals)interval.epoch->samples+=prefix.at(interval.end)-prefix.at(interval.begin);
    const auto wb_started=thread_cpu_ns();size_t wb_offset=0;
    if(batch->staged_readback)batch->slot->readback.invalidate();
    for(auto& [address,mirror]:batch->mirrors){
        auto* source=batch->staged_readback?static_cast<uint8_t*>(batch->slot->readback.mapped)+wb_offset:mirror.buffer.mapped;
        NvnDirtyWrite write(reinterpret_cast<void*>(address),mirror.bytes);memcpy(reinterpret_cast<void*>(address),source,mirror.bytes);++mirror_writebacks;readback_copied_bytes+=mirror.bytes;wb_offset+=(mirror.bytes+15)&~size_t(15);
    }
    readback_cpu_ns+=thread_cpu_ns()-wb_started;
    batch->mirrors.clear();
    // Retirement may occur while a different slot is being recorded. Never
    // append old destructors to the new batch or reset its upload arena.
    retiring_gpu_batch=true;
    for(auto& retire:batch->retire)retire();
    batch->retire.clear();retiring_gpu_batch=false;
    release_command(batch->command);batch->command={};
    for(auto& arena:batch->slot->arenas)arena.used=0;
    batch->slot->busy=false;batch->retired=true;
    trim_resource_uploads();trim_readbacks();
}
void drain_gpu_locked(){
    while(!gpu_inflight.empty()){auto batch=gpu_inflight.front();retire_gpu_batch(batch);gpu_inflight.pop_front();}
}
void retire_gpu_through(const std::shared_ptr<GpuBatch>& target){
    if(!target||target->retired)return;
    while(!target->retired&&!gpu_inflight.empty()){auto batch=gpu_inflight.front();retire_gpu_batch(batch);gpu_inflight.pop_front();}
}
bool previous_cpu_overlap(uintptr_t address,size_t bytes){
    // Publication builds one union instead of walking every batch per read.
    // Retirement can leave stale ranges here: they only cause a conservative
    // fallback below, never a missed writer. All access uses state_mutex.
    if(dependency_union_enabled&&!inflight_write_union.overlaps(address,bytes,dependency_union_checks)){
        ++dependency_union_skips;return false;
    }
    const DirectCache<2,bool>::Key key{address,bytes};
    if(dependency_memo_enabled&&previous_read_memo.find(key)){++dependency_memo_hits;return false;}
    ++dependency_memo_misses;
    for(auto& batch:gpu_inflight){
        if(dependency_bounds_enabled&&!batch->may_write(address,bytes)){++dependency_bounds_skips;continue;}
        if(dependency_index_enabled){if(batch->write_ranges.overlaps(address,bytes,dependency_index_checks))return true;continue;}
        for(auto& [base,mirror]:batch->mirrors){++dependency_range_checks;if(overlap(address,bytes,base,mirror.bytes))return true;}
        for(auto& counter:batch->counters){++dependency_range_checks;if(overlap(address,bytes,counter.address,sizeof(NVNcounterData)))return true;}
    }
    if(dependency_memo_enabled)previous_read_memo.insert(key,true);
    return false;
}
void ensure_previous_cpu_writes(){
    for(auto& batch:gpu_inflight)if(!batch->mirrors.empty()){flush_commands();return;}
}
void ensure_batch_slot(){
    if(recording_slot)return;
    GpuSlot* available=nullptr;
    for(auto& slot:gpu_slots)if(!slot.busy){available=&slot;break;}
    if(!available){
        auto batch=gpu_inflight.front();retire_gpu_batch(batch);gpu_inflight.pop_front();
        available=batch->slot;
    }
    if(!available->occlusion){
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryCount=query_capacity;ci.queryType=VK_QUERY_TYPE_OCCLUSION;
        check(vkCreateQueryPool(device,&ci,nullptr,&available->occlusion),"GPU ring occlusion pool");
        ci.queryType=VK_QUERY_TYPE_TIMESTAMP;check(vkCreateQueryPool(device,&ci,nullptr,&available->timestamps),"GPU ring timestamp pool");
    }else ++gpu_ring_reuses;
    // Upload allocation can precede the first command in a batch.
    if(!upload_arenas.empty())ot2_host_die("GPU ring arena ownership overlap");
    upload_arenas.swap(available->arenas);recording_slot=available;available->busy=true;
    occlusion_query=available->occlusion;counter_query=available->timestamps;
}
void flush_commands(bool wait){
    FrameTraceScope trace(wait?"OT2 flush and wait":"OT2 flush asynchronous");
    materialize_clears();
    if(batch_command){
        end_rendering();
        auto batch=std::make_shared<GpuBatch>();batch->command=batch_command;batch_command={};batch->slot=recording_slot;
        if(!batch->slot)ot2_host_die("GPU batch has no owned slot");
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(batch->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        size_t readback_size=0;bool eligible=cached_readback&&!buffer_mirrors.empty();
        for(const auto& [_,m]:buffer_mirrors){const auto bytes=(m.bytes+15)&~size_t(15);if(bytes>32*1024*1024||readback_size>32*1024*1024-bytes){eligible=false;break;}readback_size+=bytes;}
        eligible&=readback_size>=4096&&readback_size<=std::min<size_t>(cache_budget.resource_bytes/4,32*1024*1024);
        if(eligible&&batch->slot->readback.ensure(readback_size)){
            size_t offset=0;for(const auto& [_,m]:buffer_mirrors){VkBufferCopy copy{m.buffer.offset,offset,(m.bytes+3)&~size_t(3)};vkCmdCopyBuffer(batch->command,m.buffer.buffer,batch->slot->readback.buffer,1,&copy);offset+=(m.bytes+15)&~size_t(15);}
            VkMemoryBarrier visible{VK_STRUCTURE_TYPE_MEMORY_BARRIER};visible.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;visible.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(batch->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&visible,0,nullptr,0,nullptr);
            batch->staged_readback=true;readback_staged_bytes+=readback_size;
        }else if(eligible)++readback_fallbacks;
        gpu_profile_end(batch->command);
        check(vkEndCommandBuffer(batch->command),"end GPU ring batch");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fi,nullptr,&batch->fence),"GPU ring fence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&batch->command;
        check(vkQueueSubmit(queue,1,&si,batch->fence),"GPU ring submit");++perf.submits;
#ifdef OT2_NVN_TESTING
        ++test_gpu_submitted;
#endif
        sample_intervals.push_back({counter_epoch,sample_reset_index,pending_queries});
        batch->queries=pending_queries;batch->counters=std::move(pending_counters);batch->intervals=std::move(sample_intervals);
        batch->software_samples=std::move(software_sample_slots);software_sample_slots.clear();
        batch->mirrors=std::move(buffer_mirrors);batch->retire=std::move(batch_retire);
        batch->write_ranges=std::move(pending_write_ranges);pending_write_ranges.clear();
        for(const auto& [base,mirror]:batch->mirrors)batch->include_write(base,mirror.bytes);
        for(const auto& counter:batch->counters)batch->include_write(counter.address,sizeof(NVNcounterData));
        pending_queries=sample_reset_index=batch_operations=0;
        pending_counters.clear();sample_intervals.clear();buffer_mirrors.clear();batch_retire.clear();
        readonly_snapshots.clear();reset_binding_cache();
        batch->slot->arenas.swap(upload_arenas);recording_slot=nullptr;
        occlusion_query=synchronous_occlusion_query;counter_query=synchronous_counter_query;
        // A new writer can invalidate a previous non-overlap result. Retiring
        // writers cannot turn a negative result positive; only publication of
        // a new immutable batch needs to advance this cache's epoch.
        previous_read_memo.clear();
        inflight_write_union.clear();
        for(const auto& previous:gpu_inflight)if(!previous->retired)inflight_write_union.include(previous->write_ranges);
        inflight_write_union.include(batch->write_ranges);
        last_gpu_batch=batch;gpu_inflight.push_back(batch);gpu_inflight_peak=std::max<uint64_t>(gpu_inflight_peak,gpu_inflight.size());
    }
    if(wait||!asynchronous_gpu)drain_gpu_locked();
}
// Called after releasing state_mutex. A full completion queue must never block
// a producer while holding the mutex required by the retirement worker.
void enqueue_gpu_completion(uint64_t sequence,std::shared_ptr<GpuBatch> batch,std::shared_ptr<void> pins={}){
    completion_queue().push([sequence,batch=std::move(batch),pins=std::move(pins)](uint64_t)mutable{
        FrameTraceScope trace("OT2 completion job");
        if(batch){
            {FrameTraceScope wait_trace("OT2 worker fence wait");check(vkWaitForFences(device,1,&batch->fence,VK_TRUE,UINT64_MAX),"GPU worker completion");}
            {std::lock_guard lock(state_mutex);retire_gpu_through(batch);}
        }
        // Releasing command pins can call object Finalize and take state_mutex.
        pins.reset();gpu_completion.complete(sequence);
    });
}
