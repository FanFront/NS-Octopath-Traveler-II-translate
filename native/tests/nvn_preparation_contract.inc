// Real resource preparation, protected guest pages and descriptor-consuming GPU
// dispatch. An unrelated page write must not rebuild otherwise identical input.
extern "C" void vkhost_test_precision_enabled(int enabled){drain_queues();std::lock_guard lock(state_mutex);reset_prepared_bindings();precise_resource_dependencies=enabled!=0;}
extern "C" int vkhost_test_precise_preparation(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();
    const size_t page=sysconf(_SC_PAGESIZE);
    auto* memory=static_cast<uint32_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(memory==MAP_FAILED||!nvn_dirty_add_range(memory,page*2))return 0;
    bool valid=true;auto saved=precise_resource_dependencies;
    Shader shader;Json meta={{"key","1122334455667788"},{"cbufs",Json::array({{{"binding",1},{"slot",3}}})},{"sbufs",Json::array()},{"textures",Json::array()},{"images",Json::array()}};
    shader.resources=std::make_unique<ShaderReflection>(meta);
    const auto saved_uniform=render_state.uniforms[5][0];
    render_state.uniforms[5][0].addr=reinterpret_cast<uintptr_t>(memory);render_state.uniforms[5][0].size=16;
    auto result=make_buffer(nullptr,96,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,true);
    const auto module=clear_module(descriptor_probe_comp);
    for(bool precise:{false,true}){
        precise_resource_dependencies=precise;reset_prepared_bindings();batching=true;
        {NvnDirtyWrite write(memory,16);for(unsigned i=0;i<4;i++)memory[i]=10+i;}
        nvn_dirty_protect(memory,page*2);memset(result.mapped,0xcc,result.size);
        auto output=make_buffer(nullptr,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        auto first=std::make_unique<DrawResources>();prepare_stage_groups(*first,5,shader);
        first->adopt(2,output,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);first->finish();remember_prepared_bindings(*first,true,false);
        auto dispatch=[&](DrawResources& r,size_t offset){
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=r.layout;
            auto pipeline=cached_pipeline(ci,r.bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,r.set);vkCmdDispatch(cmd,1,1,1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            VkBufferCopy copy{output.offset,offset,16};vkCmdCopyBuffer(cmd,output.buffer,result.buffer,1,&copy);finish_commands(cmd);
        };
        dispatch(*first,0);auto hits=group_hits;
        static_cast<volatile uint32_t*>(memory)[page/4]=7;
        auto second=std::make_unique<DrawResources>();bool reused=restore_prepared_bindings(*second,true);valid&=reused==precise;
        DrawResources group;prepare_stage_groups(group,5,shader);
        valid&=(group_hits>hits)==(precise&&grouped_resources&&reuse_resource_preparation&&reuse_resource_objects);
        if(!reused){prepare_stage_groups(*second,5,shader);second->adopt(2,output,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);second->finish();}
        dispatch(*second,32);remember_prepared_bindings(*second,true,false);
        // A relevant write must still reject the full cache. Rebuild after a
        // recording boundary, as required by the guest input snapshot contract.
        memory[0]=50;DrawResources changed;valid&=!restore_prepared_bindings(changed,true);
        flush_commands();first.reset();second.reset();group.reset();batching=true;
        auto third=std::make_unique<DrawResources>();prepare_stage_groups(*third,5,shader);
        output=make_buffer(nullptr,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);third->adopt(2,output,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);third->finish();dispatch(*third,64);
        // Publishing a prepared snapshot after a source write must not advance
        // its stamp beyond the bytes actually copied into the GPU buffer.
        if(precise)memory[1]=99;
        remember_prepared_bindings(*third,true,false);
        if(precise){auto stale=restore_prepared_bindings(changed,true);if(stale)printf("PREPARATION_STALE published=%llu now=%llu ranges=%zu\n",(unsigned long long)prepared_bindings.dirty,(unsigned long long)nvn_dirty_now(),prepared_bindings.dependencies.ranges.size());valid&=!stale;}
        nvn_dirty_remove_range(memory);nvn_dirty_add_range(memory,page*2);
        valid&=!restore_prepared_bindings(changed,true);flush_commands();third.reset();batching=false;
        constexpr uint32_t increment[]={3,5,7,11};auto* values=static_cast<uint32_t*>(result.mapped);
        for(unsigned row=0;row<3;row++)for(unsigned i=0;i<8;i++){auto expected=i<4?((row==2&&i==0?50:10+i)+increment[i]):0xcccccccc;if(values[row*8+i]!=expected)printf("PREPARATION_PIXEL row=%u col=%u actual=%u expected=%u\n",row,i,values[row*8+i],expected);valid&=values[row*8+i]==expected;}
        printf("PREPARATION_DEPENDENCIES precise=%u unrelated_reused=%u valid=%u\n",precise,reused,valid);
    }
    reset_prepared_bindings();render_state.uniforms[5][0]=saved_uniform;precise_resource_dependencies=saved;
    destroy_buffer(result);nvn_dirty_remove_range(memory);munmap(memory,page*2);return valid;
}
