// State belongs to a recording, never to the lifetime of a recycled Vk handle.
// All graphics pipelines in this backend use dynamic viewport and scissor.
// Set 0 is immutable while pending. Dynamic offsets are per draw and form
// part of binding state even when the same descriptor set remains bound.
bool command_state_enabled=true;
struct CommandState {
    VkCommandBuffer command{};
    VkPipeline pipelines[2]{};
    VkPipelineLayout layouts[2]{};
    VkDescriptorSet sets[2]{};
    std::vector<uint32_t> dynamic_offsets[2];
    bool viewport_valid=false,scissor_valid=false;
    VkViewport viewport{};VkRect2D scissor{};
    struct Buffer {VkBuffer buffer{};VkDeviceSize offset{};};
    Buffer vertices[16]{},index{};VkIndexType index_type{};
} command_state;
struct CommandStateStats {uint64_t emitted[6]{},skipped[6]{};} command_state_stats;
void command_state_reset(VkCommandBuffer cmd){command_state={};command_state.command=cmd;}
void command_state_select(VkCommandBuffer cmd){if(command_state.command!=cmd)command_state_reset(cmd);}
bool command_state_emit(unsigned kind,bool same){
    if(command_state_enabled&&same){++command_state_stats.skipped[kind];return false;}
    ++command_state_stats.emitted[kind];return true;
}
unsigned command_state_point(VkPipelineBindPoint point){
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)return 0;
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE)return 1;
    ot2_host_die("Unsupported command binding point");
}
void bind_pipeline(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipeline pipeline){
    command_state_select(cmd);auto& current=command_state.pipelines[command_state_point(point)];
    if(command_state_emit(0,current==pipeline))vkCmdBindPipeline(cmd,point,pipeline);current=pipeline;
}
void bind_descriptors(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipelineLayout layout,VkDescriptorSet set,std::span<const uint32_t> offsets={}){
    command_state_select(cmd);auto index=command_state_point(point);
    auto& previous=command_state.dynamic_offsets[index];
    bool same=previous.size()==offsets.size()&&std::equal(previous.begin(),previous.end(),offsets.begin());
    if(command_state_emit(1,command_state.layouts[index]==layout&&command_state.sets[index]==set&&same))vkCmdBindDescriptorSets(cmd,point,layout,0,1,&set,offsets.size(),offsets.data());
    previous.assign(offsets.begin(),offsets.end());
    command_state.layouts[index]=layout;command_state.sets[index]=set;
}
void bind_viewport(VkCommandBuffer cmd,const VkViewport& viewport){
    command_state_select(cmd);
    if(command_state_emit(2,command_state.viewport_valid&&!memcmp(&command_state.viewport,&viewport,sizeof(viewport))))vkCmdSetViewport(cmd,0,1,&viewport);
    command_state.viewport=viewport;command_state.viewport_valid=true;
}
void bind_scissor(VkCommandBuffer cmd,const VkRect2D& scissor){
    command_state_select(cmd);
    if(command_state_emit(3,command_state.scissor_valid&&!memcmp(&command_state.scissor,&scissor,sizeof(scissor))))vkCmdSetScissor(cmd,0,1,&scissor);
    command_state.scissor=scissor;command_state.scissor_valid=true;
}
void bind_vertex(VkCommandBuffer cmd,unsigned binding,VkBuffer buffer,VkDeviceSize offset){
    command_state_select(cmd);if(binding>=std::size(command_state.vertices))ot2_host_die("Vertex binding outside supported range");
    auto& current=command_state.vertices[binding];
    if(command_state_emit(4,current.buffer==buffer&&current.offset==offset))vkCmdBindVertexBuffers(cmd,binding,1,&buffer,&offset);
    current={buffer,offset};
}
void bind_index(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,VkIndexType type){
    command_state_select(cmd);auto& current=command_state.index;
    if(command_state_emit(5,current.buffer==buffer&&current.offset==offset&&command_state.index_type==type))vkCmdBindIndexBuffer(cmd,buffer,offset,type);
    current={buffer,offset};command_state.index_type=type;
}
void command_state_report(){
    const auto& e=command_state_stats.emitted;const auto& s=command_state_stats.skipped;
    stats_log("COMMAND_STATE enabled=%u emitted=%llu skipped=%llu pipeline=%llu descriptor=%llu viewport=%llu scissor=%llu vertex=%llu index=%llu",command_state_enabled,(unsigned long long)(e[0]+e[1]+e[2]+e[3]+e[4]+e[5]),(unsigned long long)(s[0]+s[1]+s[2]+s[3]+s[4]+s[5]),(unsigned long long)s[0],(unsigned long long)s[1],(unsigned long long)s[2],(unsigned long long)s[3],(unsigned long long)s[4],(unsigned long long)s[5]);
    command_state_stats={};command_state_enabled=!std::filesystem::exists(shader_directory+"/no-command-state-cache");
}
