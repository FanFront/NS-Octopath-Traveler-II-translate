// Persistent readonly UBO/SSBO/texel uploads. Exact ranges have independent
// snapshots; slices are naturally descriptor-aligned even for unaligned guest
// addresses. A slice is mutable only while its cache is its sole owner.
// Kept as an opt-in experiment: actual Adreno A/B still regressed after
// descriptor lifetime repair and large-range admission. Never enable from
// upload-byte savings alone; the ordinary arena remains the shipping path.
bool resource_uploads=false;
uint64_t resource_upload_hits=0,resource_upload_misses=0,resource_upload_partial=0,resource_upload_bytes=0,resource_upload_saved=0;
size_t resource_resident_bytes=0;
void invalidate_descriptor_contents();
struct ResourceSlab {
    GpuBuffer buffer;size_t stride{};std::vector<size_t> free;
    ~ResourceSlab(){invalidate_descriptor_contents();invalidate_buffer_views(buffer.buffer);vkUnmapMemory(device,buffer.memory);vkDestroyBuffer(device,buffer.buffer,nullptr);vkFreeMemory(device,buffer.memory,nullptr);resource_resident_bytes-=buffer.size;}
};
std::map<size_t,std::vector<std::weak_ptr<ResourceSlab>>> resource_slabs;
struct ResourceUpload {
    GpuBuffer buffer;uint64_t stamp{},seen{};
    std::list<ReadonlySnapshots::Key>::iterator lru;
};
struct ResourceKeyHash {
    size_t operator()(const ReadonlySnapshots::Key& key)const {
        uint64_t h=14695981039346656037ull;
        for(auto value:key){h^=value;h*=1099511628211ull;}
        h^=h>>33;h*=0xff51afd7ed558ccdull;return h^(h>>33);
    }
};
std::list<ReadonlySnapshots::Key> resource_lru;
std::unordered_map<ReadonlySnapshots::Key,ResourceUpload,ResourceKeyHash> resource_upload_cache;
void erase_resource_upload(decltype(resource_upload_cache)::iterator i){resource_lru.erase(i->second.lru);resource_upload_cache.erase(i);}
bool trim_resource_uploads(size_t incoming=0){
    // Full scans only for a budget change/pressure. Admission below uses a
    // bounded number of candidates so live slices cannot cause quadratic work.
    for(auto i=resource_lru.begin();i!=resource_lru.end()&&
        (resource_upload_cache.size()+incoming>cache_budget.resource_entries||resource_resident_bytes>cache_budget.resource_bytes);){
        auto found=resource_upload_cache.find(*i++);
        if(found->second.buffer.persistent.use_count()==1)erase_resource_upload(found);
    }
    return resource_upload_cache.size()+incoming<=cache_budget.resource_entries&&resource_resident_bytes<=cache_budget.resource_bytes;
}
GpuBuffer allocate_resource_slice(size_t bytes,VkBufferUsageFlags usage,const void* source){
    const size_t stride=std::bit_ceil(std::max(bytes,size_t(upload_alignment)));
    std::shared_ptr<ResourceSlab> slab;
    auto& candidates=resource_slabs[stride];
    for(auto i=candidates.begin();i!=candidates.end();){
        auto candidate=i->lock();if(!candidate){i=candidates.erase(i);continue;}
        if(candidate->stride==stride&&!candidate->free.empty()){slab=std::move(candidate);break;}++i;
    }
    if(!slab){
        const size_t size=std::max<size_t>(64*1024,stride);
        if(size>cache_budget.resource_bytes)return {};
        // A size class that stops being used must not monopolize admission.
        // Release at most 64 idle slices per miss; busy versions stay leased.
        unsigned tried=0;
        for(auto i=resource_lru.begin();i!=resource_lru.end()&&resource_resident_bytes>cache_budget.resource_bytes-size&&tried++<64;){
            auto f=resource_upload_cache.find(*i++);if(f->second.buffer.persistent.use_count()==1)erase_resource_upload(f);
        }
        if(resource_resident_bytes>cache_budget.resource_bytes-size)return {};
        GpuBuffer b{};b.size=size;b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=b.usage;
        if(vkCreateBuffer(device,&ci,nullptr,&b.buffer)!=VK_SUCCESS)return {};
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,b.buffer,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(vkAllocateMemory(device,&ai,nullptr,&b.memory)!=VK_SUCCESS){vkDestroyBuffer(device,b.buffer,nullptr);cache_allocation_pressure();return {};}
        auto result=vkBindBufferMemory(device,b.buffer,b.memory,0);
        if(result==VK_SUCCESS)result=vkMapMemory(device,b.memory,0,size,0,&b.mapped);
        if(result!=VK_SUCCESS){vkDestroyBuffer(device,b.buffer,nullptr);vkFreeMemory(device,b.memory,nullptr);cache_allocation_pressure();return {};}
        slab=std::make_shared<ResourceSlab>();slab->buffer=b;slab->stride=stride;
        for(size_t offset=0;offset<size;offset+=stride)slab->free.push_back(offset);
        resource_resident_bytes+=size;candidates.push_back(slab);++perf.buffer_allocs;
    }
    const auto offset=slab->free.back();slab->free.pop_back();auto b=slab->buffer;
    b.offset=offset;b.mapped=static_cast<uint8_t*>(b.mapped)+offset;b.size=bytes;b.source=source;b.usage=usage;b.descriptor_stable=true;
    b.persistent=std::shared_ptr<void>(b.mapped,[slab=std::move(slab),offset](void*){slab->free.push_back(offset);});return b;
}
GpuBuffer persistent_resource(const void* data,size_t size,VkBufferUsageFlags usage,size_t minimum){
    // Small, frequently rewritten uniforms cost more in protection faults,
    // lifetime bookkeeping and descriptor churn than their memcpy. Retain the
    // recording arena below 4 KiB; persistent caching targets substantial reads.
    if(!resource_uploads||!batching||!data||size<4096||size>8*1024*1024||
       !(usage&(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)))return {};
    size_t allocation=std::max<size_t>({size,minimum,16});
    if(compact_uniforms&&robust_uniform_ranges&&usage==VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
        allocation=(std::max<size_t>(size,16)+robust_uniform_alignment-1)&~(robust_uniform_alignment-1);
    ReadonlySnapshots::Key key{uintptr_t(data),size,usage,minimum};auto found=resource_upload_cache.find(key);const auto now=nvn_dirty_now();
    if(found!=resource_upload_cache.end()){
        auto& e=found->second;
        if(e.seen==now||!nvn_dirty_since(data,size,e.stamp)){
            e.seen=now;resource_lru.splice(resource_lru.end(),resource_lru,e.lru);++resource_upload_hits;resource_upload_saved+=size;return e.buffer;
        }
        if(e.buffer.persistent.use_count()==1){
            const auto stamp=nvn_dirty_now();if(!nvn_dirty_protect(data,size))return {};
            PerfScope timing(perf.buffer_ns);size_t cursor=0,offset=0,bytes=0;
            while(nvn_dirty_next_range(data,size,e.stamp,&cursor,&offset,&bytes)){
                memcpy(static_cast<uint8_t*>(e.buffer.mapped)+offset,static_cast<const uint8_t*>(data)+offset,bytes);
                resource_upload_bytes+=bytes;perf.buffer_bytes+=bytes;
            }
            e.stamp=stamp;e.seen=stamp;resource_lru.splice(resource_lru.end(),resource_lru,e.lru);++resource_upload_partial;return e.buffer;
        }
        // A recorded draw owns the previous bytes. Allocate a new version;
        // never mutate or recycle its slice before that draw's fence retires.
        erase_resource_upload(found);
    }
    // Existing entries already have a tracked pool lifetime. Removal changes
    // the dirty epoch and protect() rejects an untracked replacement; only a
    // new admission needs the extra full range lookup.
    if(!nvn_dirty_tracked(data,size))return {};
    unsigned tried=0;for(auto i=resource_lru.begin();i!=resource_lru.end()&&resource_upload_cache.size()>=cache_budget.resource_entries&&tried++<64;){auto f=resource_upload_cache.find(*i++);if(f->second.buffer.persistent.use_count()==1)erase_resource_upload(f);}
    if(resource_upload_cache.size()>=cache_budget.resource_entries)return {};
    const auto stamp=nvn_dirty_now();if(!nvn_dirty_protect(data,size))return {};
    auto b=allocate_resource_slice(allocation,usage,data);if(!b.buffer)return {};
    {PerfScope timing(perf.buffer_ns);memcpy(b.mapped,data,size);if(allocation>size)memset(static_cast<uint8_t*>(b.mapped)+size,0,allocation-size);}
    resource_upload_bytes+=size;perf.buffer_bytes+=size;if(usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)perf.uniform_bytes+=size;else perf.storage_bytes+=size;
    resource_lru.push_back(key);resource_upload_cache.emplace(key,ResourceUpload{b,stamp,stamp,std::prev(resource_lru.end())});++resource_upload_misses;return b;
}
