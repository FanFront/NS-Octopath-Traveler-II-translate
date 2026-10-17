extern "C" uint64_t vkhost_test_command_index(int enabled){drain_queues();std::lock_guard lock(state_mutex);expanded_pass_planning=enabled!=0;return dead_attachment_stores;}
extern "C" int vkhost_test_stream_uploads(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();reset_prepared_bindings();
    const size_t page=sysconf(_SC_PAGESIZE),bytes=page*4;
    auto* source=static_cast<uint8_t*>(mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(source==MAP_FAILED)return 0;memset(source,0x31,bytes);nvn_dirty_add_range(source,bytes);
    const auto saved_budget=cache_budget;const auto saved_enabled=resource_uploads;
    resource_uploads=true;batching=true;bool valid=true;
    auto output=make_buffer(nullptr,bytes*3+64,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,true);memset(output.mapped,0xcc,output.size);
    VkEvent event{};VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(vkCreateEvent(device,&ei,nullptr,&event),"resource hold");
    auto copy=[&](size_t offset,bool hold){
        auto b=readonly_buffer(source,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);valid&=bool(b.persistent);
        auto cmd=begin_commands();if(hold)vkCmdWaitEvents(cmd,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,nullptr,0,nullptr,0,nullptr);
        VkBufferCopy region{b.offset,offset,bytes};vkCmdCopyBuffer(cmd,b.buffer,output.buffer,1,&region);finish_commands(cmd);
        batch_retire.push_back([b]()mutable{destroy_buffer(b);});flush_commands(false);
    };
    const auto hits=resource_upload_hits;copy(0,true);
    {NvnDirtyWrite write(source,bytes);memset(source,0x62,bytes);}
    copy(bytes,false);copy(bytes*2,false);valid&=resource_upload_hits>hits;
    auto tiny=saved_budget;tiny.resource_bytes=tiny.resource_entries=0;apply_cache_budget(tiny);
    valid&=resource_resident_bytes>0;
    check(vkSetEvent(device,event),"resource release");flush_commands();
    trim_resource_uploads();valid&=resource_upload_cache.empty()&&resource_resident_bytes==0;apply_cache_budget(saved_budget);
    auto* result=static_cast<uint8_t*>(output.mapped);
    for(size_t i=0;i<bytes*3;++i)valid&=result[i]==(i<bytes?0x31:0x62);
    for(size_t i=bytes*3;i<output.size;++i)valid&=result[i]==0xcc;
    // No submission owns this slice: a single changed page can be updated in
    // place. An unaligned guest address still has an aligned Vulkan offset.
    const size_t length=bytes-10;
    auto first=persistent_resource(source+3,length,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0);
    valid&=bool(first.persistent)&&first.offset%upload_alignment==0;
    auto handle=first.buffer;auto offset=first.offset;destroy_buffer(first);
    source[page+7]=0xa9;const auto uploaded=resource_upload_bytes,partial=resource_upload_partial;
    auto second=persistent_resource(source+3,length,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0);
    valid&=second.buffer==handle&&second.offset==offset&&resource_upload_partial==partial+1&&resource_upload_bytes-uploaded==page;
    valid&=!memcmp(second.mapped,source+3,length);destroy_buffer(second);
    nvn_dirty_remove_range(source);memset(source,0x94,bytes);nvn_dirty_add_range(source,bytes);
    auto replaced=persistent_resource(source+3,length,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0);valid&=!memcmp(replaced.mapped,source+3,length);destroy_buffer(replaced);
    resource_uploads=false;valid&=!persistent_resource(source,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0).buffer;
    batching=false;destroy_buffer(output);vkDestroyEvent(device,event,nullptr);resource_upload_cache.clear();resource_lru.clear();valid&=resource_resident_bytes==0;
    nvn_dirty_remove_range(source);munmap(source,bytes);resource_uploads=saved_enabled;
    printf("PASS resource slab versions/partial pages/unaligned source/pressure/guards: %u\n",valid);return valid;
}
extern "C" int vkhost_test_cached_readback(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();
    const size_t page=sysconf(_SC_PAGESIZE),bytes=page*4;
    auto* memory=static_cast<uint8_t*>(mmap(nullptr,bytes+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(memory==MAP_FAILED)return 0;nvn_dirty_add_range(memory,bytes+page);bool valid=true;const auto previous=cached_readback;
    for(bool enabled:{false,true}){
        {NvnDirtyWrite write(memory,bytes+page);memset(memory,0xcc,bytes+page);}nvn_dirty_protect(memory,bytes+page);
        auto stamp=nvn_dirty_now(),staged=readback_staged_bytes;cached_readback=enabled;batching=building_mirrors=true;
        auto b=writable_buffer(memory,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);building_mirrors=false;
        auto cmd=begin_commands();vkCmdFillBuffer(cmd,b.buffer,b.offset,bytes,0x41372519);finish_commands(cmd);flush_commands();batching=false;
        auto* words=reinterpret_cast<uint32_t*>(memory);for(size_t i=0;i<bytes/4;++i)valid&=words[i]==0x41372519;
        for(size_t i=bytes;i<bytes+page;++i)valid&=memory[i]==0xcc;
        valid&=nvn_dirty_since(memory,bytes,stamp)&&!nvn_dirty_since(memory+bytes,page,stamp);
        if(!enabled)valid&=readback_staged_bytes==staged;
        printf("READBACK_CONTRACT enabled=%u staged_bytes=%llu pixels=%u\n",enabled,(unsigned long long)(readback_staged_bytes-staged),valid);
    }
    nvn_dirty_remove_range(memory);munmap(memory,bytes+page);cached_readback=previous;return valid;
}

extern "C" int vkhost_test_resource_descriptors(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();
    const auto saved_budget=cache_budget;const auto saved_limit=max_idle_descriptors;
    const bool saved_uploads=resource_uploads,saved_contents=retain_descriptor_contents,saved_delta=incremental_descriptors;
    const bool saved_reuse=reuse_descriptors,saved_bindings=reuse_bindings;
    resource_uploads=retain_descriptor_contents=incremental_descriptors=reuse_descriptors=reuse_bindings=true;
    max_idle_descriptors=0;trim_descriptors();max_idle_descriptors=64;
    const size_t page=sysconf(_SC_PAGESIZE);
    auto* source=static_cast<uint32_t*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(source==MAP_FAILED)return 0;
    for(size_t i=0;i<page/4;++i)source[i]=100+i;
    nvn_dirty_add_range(source,page);
    VkEvent event{};VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(vkCreateEvent(device,&ei,nullptr,&event),"descriptor resource hold");
    const auto module=clear_module(descriptor_probe_comp);bool valid=true;
    auto submit=[&](bool hold){
        batching=true;auto out=make_buffer(nullptr,256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);memset(out.mapped,0xcc,256);
        auto r=std::make_unique<DrawResources>();r->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;
        valid&=!persistent_resource(source,64,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,0).buffer;
        r->adopt(1,readonly_buffer(source,page,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        valid&=r->buffers.back().descriptor_stable;r->adopt(2,out,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);r->finish();
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=r->layout;
        auto pipeline=cached_pipeline(ci,r->bindings);auto cmd=begin_commands();
        if(hold)vkCmdWaitEvents(cmd,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,nullptr,0,nullptr,0,nullptr);
        bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,r->layout,r->set);vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);
        auto* owner=r.release();batch_retire.push_back([owner]{delete owner;});flush_commands(false);return out;
    };
    auto check_output=[&](const GpuBuffer& out,uint32_t base){
        const auto* result=static_cast<const uint32_t*>(out.mapped);constexpr uint32_t increments[]={3,5,7,11};
        for(unsigned i=0;i<64;++i)valid&=result[i]==(i<4?base+i+increments[i]:0xcccccccc);
    };
    const auto old=submit(true);
    {NvnDirtyWrite write(source,64);for(unsigned i=0;i<16;++i)source[i]=200+i;}
    const auto changed=submit(false);
    check(vkSetEvent(device,event),"descriptor resource release");flush_commands();
    check_output(old,100);check_output(changed,200);
    const auto hits=descriptor_contents_hits,skipped=descriptor_delta_skipped;
    for(unsigned round=0;round<8;++round){auto out=submit(false);flush_commands();check_output(out,200);}
    valid&=descriptor_contents_hits>hits||descriptor_delta_skipped>skipped;
    // Idle descriptors hold no slice lease. Destroying the last slab must
    // invalidate both exact contents and delta values before handle reuse.
    const auto epoch=descriptor_contents_epoch;valid&=!idle_descriptor_contents.empty();
    auto tiny=saved_budget;tiny.resource_bytes=tiny.resource_entries=0;apply_cache_budget(tiny);
    valid&=resource_resident_bytes==0&&descriptor_contents_epoch>epoch&&idle_descriptor_contents.empty();
    for(auto* slot=idle_descriptors.first;slot;slot=slot->idle_links[0].next)valid&=slot->values_epoch!=descriptor_contents_epoch;
    apply_cache_budget(saved_budget);
    {NvnDirtyWrite write(source,64);for(unsigned i=0;i<16;++i)source[i]=300+i;}
    auto replacement=submit(false);flush_commands();check_output(replacement,300);
    batching=false;reset_binding_cache();readonly_snapshots.clear();resource_upload_cache.clear();resource_lru.clear();
    nvn_dirty_remove_range(source);munmap(source,page);vkDestroyEvent(device,event,nullptr);
    printf("RESOURCE_DESCRIPTOR_CONTRACT contents_hits=%llu delta_skipped=%llu held_versions_and_eviction=%u\n",(unsigned long long)(descriptor_contents_hits-hits),(unsigned long long)(descriptor_delta_skipped-skipped),valid);
    resource_uploads=saved_uploads;retain_descriptor_contents=saved_contents;incremental_descriptors=saved_delta;reuse_descriptors=saved_reuse;reuse_bindings=saved_bindings;max_idle_descriptors=saved_limit;
    return valid;
}
