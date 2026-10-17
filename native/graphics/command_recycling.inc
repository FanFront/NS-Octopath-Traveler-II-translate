// Only retired command buffers enter this list. GPU fences deliberately keep
// their original lifetime: an older completion worker may still wait on them.
// state_mutex protects both this list and the shared Vulkan command pool.
bool recycle_commands=false;
std::vector<VkCommandBuffer> idle_commands;
uint64_t command_allocations=0,command_reuses=0,command_frees=0;
void trim_commands(){
    if(idle_commands.empty())return;
    vkFreeCommandBuffers(device,command_pool,idle_commands.size(),idle_commands.data());
    command_frees+=idle_commands.size();idle_commands.clear();
}
VkCommandBuffer acquire_command(){
    if(recycle_commands&&!idle_commands.empty()){
        auto cmd=idle_commands.back();idle_commands.pop_back();
        check(vkResetCommandBuffer(cmd,0),"reset retired command");++command_reuses;return cmd;
    }
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=command_pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
    VkCommandBuffer cmd;check(vkAllocateCommandBuffers(device,&a,&cmd),"command allocation");++command_allocations;return cmd;
}
void release_command(VkCommandBuffer cmd){
    // Three in-flight ring slots and one synchronous transfer need at most four
    // reusable objects. Overflow frees immediately; background trim frees all.
    if(recycle_commands&&idle_commands.size()<4){idle_commands.push_back(cmd);return;}
    vkFreeCommandBuffers(device,command_pool,1,&cmd);++command_frees;
}
