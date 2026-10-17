// Real GPU workload above the former 1,024-slot idle limit. Every dispatch has
// distinct inputs and output guards; leases retire through the normal fence.
static bool descriptor_idle_contract();
extern "C" int vkhost_test_descriptor_delta_order(){
    vkhost_init();std::lock_guard lock(state_mutex);flush_commands();reset_binding_cache();
    const bool old_batching=batching,old_linear=linear_descriptor_delta,old_bindings=reuse_bindings;
    const bool old_contents=retain_descriptor_contents,old_working=retain_descriptor_working_set;
    const bool old_incremental=incremental_descriptors,old_templates=use_descriptor_templates;
    const size_t old_limit=max_idle_descriptors;
    max_idle_descriptors=0;trim_descriptors();max_idle_descriptors=1;
    reuse_bindings=false;retain_descriptor_contents=incremental_descriptors=true;retain_descriptor_working_set=false;use_descriptor_templates=false;
    batching=false;constexpr size_t stride=256,words=stride/4;
    std::array<uint32_t,words*3> source{};std::array<uint32_t,words> initial{};
    auto in=make_buffer(source.data(),sizeof(source),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto out=make_buffer(initial.data(),sizeof(initial),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // The fixture owns both allocations through all fences. Draw copies borrow
    // them with the same lease/stable-descriptor contract as persistent slabs.
    auto borrowed=[](GpuBuffer b){b.descriptor_stable=true;b.persistent=std::shared_ptr<void>(b.mapped,[](void*){});return b;};
    auto module=clear_module(descriptor_probe_comp);bool valid=true;batching=true;
    const auto skipped=descriptor_delta_skipped;
    for(int mode:{0,1,0,1})for(unsigned round=0;round<8;++round){
        linear_descriptor_delta=mode;
        auto* src=static_cast<uint32_t*>(in.mapped);auto* dst=static_cast<uint32_t*>(out.mapped);
        for(size_t i=0;i<source.size();++i)src[i]=1000*round+i;
        std::fill(dst,dst+words,0xdeadbeef);
        auto input=borrowed(in),output=borrowed(out);input.offset=(round%3)*stride;input.size=output.size=16;
        DrawResources resources;resources.current_stage=VK_SHADER_STAGE_COMPUTE_BIT;
        if(round%4<2){resources.adopt(1,input,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);resources.adopt(2,output,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);}
        else {resources.adopt(2,output,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);resources.adopt(1,input,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);}
        resources.finish();
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=resources.layout;
        const auto pipeline=cached_pipeline(ci,resources.bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ci.layout,resources.set);
        vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);flush_commands();
        const uint32_t add[]={3,5,7,11};
        for(size_t c=0;c<words;++c)valid&=dst[c]==(c<4?src[(round%3)*words+c]+add[c]:0xdeadbeef);
    }
    valid&=descriptor_delta_skipped>skipped;
    invalidate_descriptor_contents();destroy_buffer(in);destroy_buffer(out);
    max_idle_descriptors=old_limit;batching=old_batching;linear_descriptor_delta=old_linear;reuse_bindings=old_bindings;
    retain_descriptor_contents=old_contents;retain_descriptor_working_set=old_working;incremental_descriptors=old_incremental;use_descriptor_templates=old_templates;
    printf("DESCRIPTOR_DELTA_ORDER skipped=%llu valid=%u\n",(unsigned long long)(descriptor_delta_skipped-skipped),valid);
    return valid;
}
extern "C" int vkhost_test_descriptor_blocks(){
    vkhost_init();std::lock_guard lock(state_mutex);flush_commands();
    const auto saved_budget=cache_budget;const auto saved_policy=cache_budget_policy;
    const auto saved_limit=max_idle_descriptors;
    max_idle_descriptors=0;trim_descriptors();max_idle_descriptors=32768;
    const std::vector<VkDescriptorSetLayoutBinding> bindings{{1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    const auto created=descriptor_pool_creates;std::vector<DescriptorSlot*> held;std::set<VkDescriptorSet> unique;
    bool valid=descriptor_blocks.empty();
    for(size_t i=0;i<32768;++i){auto* slot=acquire_descriptors(bindings);held.push_back(slot);valid&=unique.insert(slot->set).second;}
    valid&=descriptor_pool_creates-created==512&&descriptor_blocks.size()==512;
    // Every block keeps a live set while its other sets are discarded. A pool
    // must outlive every recorded set, not merely the first retired one.
    max_idle_descriptors=0;
    for(size_t i=0;i<held.size();i+=2){release_descriptors(held[i]);held[i]=nullptr;}
    valid&=descriptor_blocks.size()==512&&descriptor_live_slots==16384&&idle_descriptors.empty();
    for(auto* slot:held)if(slot)release_descriptors(slot);
    valid&=descriptor_blocks.empty()&&descriptor_live_slots==0&&descriptor_idle_contract();
    const auto retries=descriptor_allocation_retries;test_descriptor_pool_failures=1;
    auto* recovered=acquire_descriptors(bindings);
    valid&=descriptor_allocation_retries==retries+1&&recovered->block->capacity==1&&cache_budget.pressure;
    max_idle_descriptors=0;release_descriptors(recovered);
    valid&=descriptor_blocks.empty()&&descriptor_live_slots==0;
    cache_budget_policy=saved_policy;apply_cache_budget(saved_budget);max_idle_descriptors=saved_limit;
    printf("DESCRIPTOR_BLOCKS sets=32768 pools=512 pressure_retry=1 all_reclaimed=%u\n",valid);
    return valid;
}
extern "C" int vkhost_test_descriptor_template_order(){
    vkhost_init();std::lock_guard lock(state_mutex);flush_commands();
    const bool old_batching=batching,old_templates=use_descriptor_templates,old_layout=reuse_layout_inputs;
    use_descriptor_templates=true;reuse_layout_inputs=true;batching=false;
    constexpr size_t count=8,stride=256,words=stride/4;
    std::vector<uint32_t> initial(count*words);
    auto in=make_buffer(initial.data(),initial.size()*4,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,stride);
    auto out=make_buffer(initial.data(),initial.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,stride);
    auto module=clear_module(descriptor_probe_comp);bool valid=true;
    DescriptorLayout* owner=nullptr;VkDescriptorUpdateTemplate templates[2]{};
    batching=true;
    for(unsigned round=0;round<4;++round){
        auto* src=static_cast<uint32_t*>(in.mapped);auto* dst=static_cast<uint32_t*>(out.mapped);
        for(size_t i=0;i<initial.size();++i){src[i]=round*10000+i;dst[i]=0xdeadbeef;}
        std::set<VkDescriptorSet> live;
        for(size_t i=0;i<count;++i){
            std::vector<VkDescriptorSetLayoutBinding> bindings{{1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
            DescriptorUpdateData data[2];data[0].buffer={in.buffer,in.offset+i*stride,16};data[1].buffer={out.buffer,out.offset+i*stride,16};
            const unsigned order=i%2;if(order){std::swap(bindings[0],bindings[1]);std::swap(data[0],data[1]);}
            PipelineKey key;key.bindings(bindings);VkDescriptorUpdateTemplate update{};
            auto* slot=acquire_descriptors(bindings,nullptr,nullptr,key.data,&update);
            if(!owner)owner=slot->owner;
            valid&=owner==slot->owner&&live.insert(slot->set).second&&update;
            if(templates[order])valid&=update==templates[order];templates[order]=update;
            vkUpdateDescriptorSetWithTemplate(device,slot->set,update,data);
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=owner->pipeline_layout;
            auto pipeline=cached_pipeline(ci,bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ci.layout,slot->set);
            vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);batch_retire.push_back([slot]{release_descriptors(slot);});
        }
        flush_commands();const uint32_t add[]={3,5,7,11};
        for(size_t i=0;i<count;++i)for(size_t c=0;c<words;++c)valid&=dst[i*words+c]==(c<4?src[i*words+c]+add[c]:0xdeadbeef);
    }
    valid&=templates[0]!=templates[1];destroy_buffer(in);destroy_buffer(out);
    batching=old_batching;use_descriptor_templates=old_templates;reuse_layout_inputs=old_layout;
    return valid;
}
static bool descriptor_idle_contract(){
    auto check_list=[](const DescriptorIdleList& list){
        size_t count=0;DescriptorSlot* previous=nullptr;
        for(auto* slot=list.front();slot;slot=slot->idle_links[list.link_index].next){
            if(++count>descriptor_live_slots||slot->idle_links[list.link_index].previous!=previous)return false;
            previous=slot;
        }
        return count==list.size()&&previous==list.back();
    };
    if(!check_list(idle_descriptors))return false;
    size_t per_layout=0;for(const auto& [key,layout]:descriptor_layouts){if(!check_list(layout.idle))return false;per_layout+=layout.idle.size();}
    return per_layout==idle_descriptors.size();
}
extern "C" int vkhost_test_descriptor_working_set(){
    vkhost_init();std::lock_guard lock(state_mutex);flush_commands();
    const bool old_batching=batching,old_working_set=retain_descriptor_working_set,old_contents=retain_descriptor_contents;
    const auto old_limit=max_idle_descriptors;
    constexpr size_t count=16,half=8,stride=256,words=stride/4;
    const std::vector<VkDescriptorSetLayoutBinding> bindings{
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    auto module=clear_module(descriptor_probe_comp);bool valid=true;
    for(int preserve:{0,1}){
        max_idle_descriptors=0;trim_descriptors();max_idle_descriptors=32;
        retain_descriptor_contents=true;retain_descriptor_working_set=preserve;batching=false;
        std::vector<uint32_t> initial(count*words);
        auto in=make_buffer(initial.data(),initial.size()*4,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,stride);
        auto out=make_buffer(initial.data(),initial.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,stride);
        batching=true;
        for(unsigned round=0;round<6;++round){
            auto* src=static_cast<uint32_t*>(in.mapped);auto* dst=static_cast<uint32_t*>(out.mapped);
            for(size_t i=0;i<count*words;i++){src[i]=round*10000+i;dst[i]=0xdeadbeef;}
            auto creates=descriptor_pool_creates,nodes=descriptor_contents_node_creates;unsigned updates=0;std::set<VkDescriptorSet> live;
            const size_t first=(round%2)*half;
            for(size_t i=first;i<first+half;i++){
                VkDescriptorBufferInfo infos[]={{in.buffer,in.offset+i*stride,16},{out.buffer,out.offset+i*stride,16}};
                PipelineKey key;key.add(uint64_t(0x574f524b534554));key.bindings(bindings);
                for(const auto& info:infos){key.add(info.buffer);key.add(info.offset);key.add(info.range);}
                bool unchanged=false;auto* slot=acquire_descriptors(bindings,&key.data,&unchanged);valid&=live.insert(slot->set).second;
                if(!unchanged){
                    VkWriteDescriptorSet writes[2]{};
                    for(unsigned b=0;b<2;b++){auto& w=writes[b];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=slot->set;w.dstBinding=b+1;w.descriptorCount=1;w.descriptorType=bindings[b].descriptorType;w.pBufferInfo=&infos[b];}
                    vkUpdateDescriptorSets(device,2,writes,0,nullptr);++updates;slot->contents=key.data;slot->contents_epoch=descriptor_contents_epoch;
                }
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=slot->owner->pipeline_layout;
                auto pipeline=cached_pipeline(ci,bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ci.layout,slot->set);
                vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);batch_retire.push_back([slot]{release_descriptors(slot);});
            }
            flush_commands();const uint32_t increments[]={3,5,7,11};
            for(size_t i=0;i<count;i++)for(size_t c=0;c<words;c++)valid&=dst[i*words+c]==(i>=first&&i<first+half&&c<4?src[i*words+c]+increments[c]:0xdeadbeef);
            if(round>=2)valid&=preserve?(updates==0&&descriptor_pool_creates==creates):updates==half;
            if(round>=2)valid&=descriptor_contents_node_creates==nodes;
            valid&=descriptor_idle_contract();
            valid&=descriptor_live_slots<=32;
            printf("DESCRIPTOR_WORKING_SET enabled=%d round=%u updates=%u created=%llu live=%zu valid=%u\n",preserve,round,updates,(unsigned long long)(descriptor_pool_creates-creates),descriptor_live_slots,valid);
        }
        invalidate_descriptor_contents();destroy_buffer(in);destroy_buffer(out);
    }
    max_idle_descriptors=old_limit;retain_descriptor_working_set=old_working_set;retain_descriptor_contents=old_contents;batching=old_batching;
    return valid;
}
extern "C" int vkhost_test_descriptor_capacity() {
    vkhost_init();std::lock_guard lock(state_mutex);flush_commands();
    const bool previous_batching=batching;batching=true;
    constexpr size_t count=2049,stride=256,words=stride/4;
    std::vector<uint32_t> input(count*words),output(count*words);
    static VkShaderModule module{};if(!module)module=clear_module(descriptor_probe_comp);
    const std::vector<VkDescriptorSetLayoutBinding> bindings{
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    bool valid=true;
    for(unsigned round=0;round<3;++round){
        for(size_t i=0;i<count;++i)for(size_t c=0;c<4;++c)input[i*words+c]=round*100000+i*16+c;
        std::fill(output.begin(),output.end(),0xdeadbeef);
        auto in=make_buffer(input.data(),input.size()*4,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,stride);
        auto out=make_buffer(output.data(),output.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,stride);
        auto before=descriptor_pool_creates;
        std::set<VkDescriptorSet> live;
        for(size_t i=0;i<count;++i){
            auto* slot=acquire_descriptors(bindings);valid&=live.insert(slot->set).second;
            VkDescriptorBufferInfo infos[]={{in.buffer,in.offset+i*stride,16},{out.buffer,out.offset+i*stride,16}};
            VkWriteDescriptorSet writes[2]{};
            for(unsigned b=0;b<2;++b){auto& w=writes[b];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=slot->set;w.dstBinding=b+1;w.descriptorCount=1;w.descriptorType=bindings[b].descriptorType;w.pBufferInfo=&infos[b];}
            vkUpdateDescriptorSets(device,2,writes,0,nullptr);
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=slot->owner->pipeline_layout;
            auto pipeline=cached_pipeline(ci,bindings);auto cmd=begin_commands();
            bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ci.layout,slot->set);
            vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);batch_retire.push_back([slot]{release_descriptors(slot);});
        }
        flush_commands();
        const uint32_t increments[]={3,5,7,11};auto* result=static_cast<const uint32_t*>(out.mapped);
        for(size_t i=0;i<count;++i)for(size_t c=0;c<words;++c)
            valid&=result[i*words+c]==(c<4?input[i*words+c]+increments[c]:0xdeadbeef);
        auto created=descriptor_pool_creates-before;
        printf("DESCRIPTOR_CAPACITY round=%u draws=%zu created=%llu idle=%zu valid=%u\n",round,count,(unsigned long long)created,idle_descriptors.size(),valid);
        if(round&&created)valid=false;
        valid&=descriptor_idle_contract();
        destroy_buffer(in);destroy_buffer(out);
    }
    batching=previous_batching;return valid;
}
