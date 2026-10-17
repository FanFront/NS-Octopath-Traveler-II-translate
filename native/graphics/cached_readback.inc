// One reusable readback allocation per GPU slot. The slot cannot be recorded
// again until its fence and CPU writeback complete. Failure is optional: use
// the existing direct mapped path without altering GPU/guest ordering.
bool cached_readback=true;
uint64_t readback_staged_bytes=0,readback_cpu_ns=0,readback_copied_bytes=0,readback_fallbacks=0;
struct ReadbackBuffer {
    VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};size_t size{};bool coherent{};
    void clear(){if(mapped)vkUnmapMemory(device,memory);if(buffer)vkDestroyBuffer(device,buffer,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);buffer={};memory={};mapped=nullptr;size=0;}
    bool ensure(size_t needed){
        if(size>=needed)return true;
        clear();size=(needed+65535)&~size_t(65535);
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(device,&ci,nullptr,&buffer)!=VK_SUCCESS){clear();return false;}
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<memory_properties.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&
            (memory_properties.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT)){type=i;break;}
        if(type==UINT32_MAX){clear();return false;}
        coherent=(memory_properties.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        auto result=vkAllocateMemory(device,&ai,nullptr,&memory);
        if(result==VK_SUCCESS)result=vkBindBufferMemory(device,buffer,memory,0);
        if(result==VK_SUCCESS)result=vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped);
        if(result!=VK_SUCCESS){clear();return false;}return true;
    }
    void invalidate(){if(!coherent){VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=memory;r.size=VK_WHOLE_SIZE;check(vkInvalidateMappedMemoryRanges(device,1,&r),"cached readback invalidate");}}
};
