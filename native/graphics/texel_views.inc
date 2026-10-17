// Buffer views describe a range/format, not its current contents. Upload-ring
// buffers keep their identity across reuse. Lease each view until the draw's
// actual GPU fence; only idle entries can be evicted. Access uses state_mutex.
struct TexelViewEntry {
    FixedKey<32> key;
    VkBufferView view{};
    size_t users=0;
    TexelViewEntry* previous{};
    TexelViewEntry* next{};
    VkBuffer buffer{};
    TexelViewEntry* buffer_previous{};
    TexelViewEntry* buffer_next{};
};
struct TexelViewLease {VkBufferView view{};TexelViewEntry* cached{};};
std::unordered_map<FixedKey<32>,std::unique_ptr<TexelViewEntry>,FixedKey<32>::Hash> texel_view_cache;
std::unordered_map<VkBuffer,TexelViewEntry*> texel_buffer_heads;
TexelViewEntry* texel_idle_first=nullptr;
TexelViewEntry* texel_idle_last=nullptr;
bool reuse_texel_views=true;
size_t texel_view_limit=8192;
uint64_t texel_view_hits=0,texel_view_creates=0,texel_view_evictions=0;
uint64_t texel_view_invalidation_visits=0;
void invalidate_descriptor_contents();
void unlink_idle_texel_view(TexelViewEntry* entry){
    if(entry->previous)entry->previous->next=entry->next;else texel_idle_first=entry->next;
    if(entry->next)entry->next->previous=entry->previous;else texel_idle_last=entry->previous;
    entry->previous=entry->next=nullptr;
}
void erase_idle_texel_view(TexelViewEntry* entry){
    if(entry->users)ot2_host_die("Attempted to evict a pending texel view");
    unlink_idle_texel_view(entry);
    if(entry->buffer_previous)entry->buffer_previous->buffer_next=entry->buffer_next;
    else if(entry->buffer_next)texel_buffer_heads.at(entry->buffer)=entry->buffer_next;
    else texel_buffer_heads.erase(entry->buffer);
    if(entry->buffer_next)entry->buffer_next->buffer_previous=entry->buffer_previous;
    // Retired descriptor sets may retain this handle, but cannot retain the
    // view itself indefinitely. Invalidate their contents/deltas before Vulkan
    // can recycle the handle. Pending draws still own a nonzero view lease.
    invalidate_descriptor_contents();
    vkDestroyBufferView(device,entry->view,nullptr);
    const auto key=entry->key;texel_view_cache.erase(key);++texel_view_evictions;
}
void trim_texel_views(){
    while(texel_view_cache.size()>texel_view_limit&&texel_idle_first)erase_idle_texel_view(texel_idle_first);
}
TexelViewLease acquire_texel_view(const VkBufferViewCreateInfo& info,bool stable){
    FixedKey<32> key;key.add(info.buffer);key.add(info.offset);key.add(info.range);key.add(info.format);key.add(info.flags);
    if(reuse_texel_views&&stable){
        if(auto found=texel_view_cache.find(key);found!=texel_view_cache.end()){
            auto* entry=found->second.get();if(!entry->users)unlink_idle_texel_view(entry);
            ++entry->users;++texel_view_hits;return {entry->view,entry};
        }
        while(texel_view_cache.size()>=texel_view_limit&&texel_idle_first)erase_idle_texel_view(texel_idle_first);
    }
    VkBufferView view{};check(vkCreateBufferView(device,&info,nullptr,&view),"texel buffer view");++texel_view_creates;
    if(reuse_texel_views&&stable&&texel_view_cache.size()<texel_view_limit){
        auto owned=std::make_unique<TexelViewEntry>();owned->key=key;owned->view=view;owned->users=1;
        auto* entry=owned.get();entry->buffer=info.buffer;
        auto& head=texel_buffer_heads[info.buffer];entry->buffer_next=head;
        if(head)head->buffer_previous=entry;head=entry;
        texel_view_cache.emplace(key,std::move(owned));return {view,entry};
    }
    return {view,nullptr}; // Unstable buffers or a fully leased cache.
}
void release_texel_view(TexelViewLease lease){
    auto* entry=lease.cached;
    if(!entry){if(lease.view)vkDestroyBufferView(device,lease.view,nullptr);return;}
    if(!entry->users||entry->view!=lease.view)ot2_host_die("Invalid texel view lease");
    if(--entry->users)return;
    entry->previous=texel_idle_last;
    if(texel_idle_last)texel_idle_last->next=entry;else texel_idle_first=entry;
    texel_idle_last=entry;
    trim_texel_views();
}
void invalidate_buffer_views(VkBuffer buffer){
    // Normally ring arenas live for the session. Keep explicit destruction
    // safe too; no cached handle may survive its backing Vulkan buffer.
    auto found=texel_buffer_heads.find(buffer);
    auto* entry=found==texel_buffer_heads.end()?nullptr:found->second;
    while(entry){auto* next=entry->buffer_next;++texel_view_invalidation_visits;erase_idle_texel_view(entry);entry=next;}
}
