// Included in the backend test build; exercise real transfer reads with an old
// immutable upload still pending while CPU writes produce its replacement.
extern "C" int vkhost_test_cache_pressure(int mode){
    // Deliberately do not drain: tests call this while a real GPU event is held.
    std::lock_guard lock(state_mutex);static CacheBudget saved;
    if(mode==1){
        saved=cache_budget;saved.descriptors=max_idle_descriptors;saved.texel_views=texel_view_limit;
        auto tiny=CacheBudget::reduced();tiny.descriptors=tiny.texel_views=0;
        tiny.geometry_bytes=tiny.geometry_entries=tiny.free_buffer_bytes=tiny.free_buffers=0;
        apply_cache_budget(tiny);
        return idle_descriptors.empty()&&!texel_idle_first&&free_buffers.empty()&&descriptor_idle_contract();
    }
    if(mode==2){trim_persistent_uploads();return descriptor_idle_contract()&&idle_descriptors.empty()&&texel_view_cache.empty()&&texel_buffer_heads.empty()&&persistent_upload_cache.empty()&&free_buffers.empty();}
    apply_cache_budget(saved);return 1;
}
extern "C" int vkhost_test_persistent_uploads(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();
    constexpr size_t bytes=8192;
    auto* source=static_cast<uint8_t*>(mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(source==MAP_FAILED)return 0;memset(source,0x31,bytes);if(!nvn_dirty_add_range(source,bytes)){munmap(source,bytes);return 0;}
    bool valid=true;const auto previous=persistent_uploads;const auto before=persistent_upload_hits;
    auto output=make_buffer(nullptr,bytes*3+64,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,true);memset(output.mapped,0xcc,output.size);
    persistent_uploads=true;batching=true;
    VkEvent event{};VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(vkCreateEvent(device,&ei,nullptr,&event),"persistent hold event");
    auto copy=[&](size_t offset,bool hold){
        auto buffer=readonly_buffer(source,bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        auto cmd=begin_commands();if(hold)vkCmdWaitEvents(cmd,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,nullptr,0,nullptr,0,nullptr);
        VkBufferCopy region{buffer.offset,offset,bytes};vkCmdCopyBuffer(cmd,buffer.buffer,output.buffer,1,&region);finish_commands(cmd);
        batch_retire.push_back([buffer]()mutable{destroy_buffer(buffer);});flush_commands(false);
    };
    copy(0,true);
    {NvnDirtyWrite write(source,bytes);memset(source,0x62,bytes);}
    copy(bytes,false);copy(bytes*2,false);
    const auto saved_budget=cache_budget;
    auto tiny=CacheBudget::reduced();tiny.geometry_bytes=tiny.geometry_entries=0;apply_cache_budget(tiny);
    valid&=!persistent_upload_cache.empty(); // Leased versions cannot be evicted.
    check(vkSetEvent(device,event),"persistent release event");flush_commands();batching=false;
    trim_persistent_uploads();valid&=persistent_upload_cache.empty()&&persistent_upload_bytes==0;
    apply_cache_budget(saved_budget);
    auto* result=static_cast<const uint8_t*>(output.mapped);
    for(size_t i=0;i<bytes*3;++i)valid&=result[i]==(i<bytes?0x31:0x62);
    for(size_t i=bytes*3;i<output.size;++i)valid&=result[i]==0xcc;
    valid&=persistent_upload_hits>before;
    // Removing/re-registering an address must invalidate the old lifetime even
    // without a write fault. Untracked memory must take the original path.
    nvn_dirty_remove_range(source);memset(source,0x94,bytes);nvn_dirty_add_range(source,bytes);batching=true;
    auto newest=readonly_buffer(source,bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    valid&=static_cast<uint8_t*>(newest.mapped)[0]==0x94;destroy_buffer(newest);readonly_snapshots.clear();
    // A recording with only host allocations still owns an arena slot.
    auto cmd=begin_commands();finish_commands(cmd);flush_commands();batching=false;
    nvn_dirty_remove_range(source);persistent_upload_cache.clear();persistent_upload_bytes=0;
    destroy_buffer(output);vkDestroyEvent(device,event,nullptr);munmap(source,bytes);persistent_uploads=previous;
    return valid;
}
extern "C" void vkhost_test_batch_optimizations(int enabled){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();reset_prepared_bindings();
    grouped_resources=versioned_pipeline_state=persistent_uploads=precise_barriers=clear_planning=prune_unused_depth=incremental_descriptors=enabled!=0;
    resource_uploads=cached_readback=enabled!=0;expanded_pass_planning=enabled!=0;nvn_dirty_use_summary(enabled);
}
extern "C" uint64_t vkhost_test_batch_work(unsigned kind){
    drain_queues();std::lock_guard lock(state_mutex);
    switch(kind){case 0:return group_hits;case 1:return descriptor_delta_skipped;case 2:return folded_clears;case 3:return pipeline_version_hits;case 4:return pruned_depth;default:return dead_attachment_stores;}
}
extern "C" int vkhost_test_descriptor_delta(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();
    const auto saved_limit=max_idle_descriptors;const auto saved_working=retain_descriptor_working_set,saved_contents=retain_descriptor_contents,saved_delta=incremental_descriptors;
    bool valid=true;const auto module=clear_module(descriptor_probe_comp);
    for(bool delta:{false,true}){
        max_idle_descriptors=0;trim_descriptors();max_idle_descriptors=32;retain_descriptor_working_set=false;retain_descriptor_contents=true;incremental_descriptors=delta;
        const auto skipped=descriptor_delta_skipped,written=descriptor_delta_written;
        for(unsigned round=0;round<6;++round){
            batching=true;const auto stride=upload_alignment;std::vector<uint32_t> input(2*stride/4);for(unsigned i=0;i<input.size();++i)input[i]=100*round+i;
            auto in=make_buffer(input.data(),input.size()*4,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            auto out=make_buffer(nullptr,256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);memset(out.mapped,0xcc,256);
            auto r=std::make_unique<DrawResources>();r->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;
            in.offset+=(round&1)*stride;in.size=16;r->adopt(1,in,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);r->adopt(2,out,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);r->finish();
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=r->layout;
            auto pipeline=cached_pipeline(ci,r->bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,r->layout,r->set);vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);
            auto* owner=r.release();batch_retire.push_back([owner]{delete owner;});flush_commands();batching=false;
            const auto* result=static_cast<const uint32_t*>(out.mapped);constexpr uint32_t increments[]={3,5,7,11};
            for(unsigned i=0;i<64;++i)valid&=result[i]==(i<4?input[(round&1)*(stride/4)+i]+increments[i]:0xcccccccc);
        }
        const auto avoided=descriptor_delta_skipped-skipped,updates=descriptor_delta_written-written;
        valid&=delta?(avoided>=10&&updates<=8):(avoided==0&&updates==18);
        printf("DESCRIPTOR_DELTA_CONTRACT enabled=%u written=%llu skipped=%llu pixels=%u\n",delta,(unsigned long long)updates,(unsigned long long)avoided,valid);
    }
    invalidate_descriptor_contents();max_idle_descriptors=saved_limit;retain_descriptor_working_set=saved_working;retain_descriptor_contents=saved_contents;incremental_descriptors=saved_delta;
    return valid;
}
