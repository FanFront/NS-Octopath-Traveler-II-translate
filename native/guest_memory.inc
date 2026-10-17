// The guest mapping registry is protected by mapping_mutex. Host pages may be
// larger than guest pages: the registry keeps 4 KiB semantics, while boundary
// host pages retain permissions needed by any live neighboring guest page.
// On such shared pages hardware cannot enforce subpage inaccessibility.
constexpr uint64_t memory_invalid_size=0xca01,memory_invalid_address=0xcc01;
constexpr uint64_t memory_invalid_current=0xd401,memory_invalid_region=0xdc01;
size_t memory_host_page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
struct TransferSource {uintptr_t base;size_t size;uint32_t handle,permission;bool closed;uint64_t serial;};
std::vector<TransferSource> transfer_sources;
uint64_t next_transfer_serial=0;
bool memory_contains(uintptr_t base,size_t capacity,uintptr_t address,size_t size){
    return size&&size<=capacity&&address>=base&&address-base<=capacity-size;
}
bool memory_overlaps(uintptr_t a,size_t na,uintptr_t b,size_t nb){return na&&nb&&a<b+nb&&b<a+na;}
struct MemoryImageRegion {uintptr_t base=0;size_t size=0;uint32_t state=0,permission=0;};
MemoryImageRegion memory_image_region(uintptr_t address){
    for(const auto& m:images){
        const auto base=reinterpret_cast<uintptr_t>(m.base);
        if(!memory_contains(base,m.size,address,1))continue;
        const auto text=base+m.text_offset,text_end=base+((m.text_offset+m.text_size+memory_host_page-1)&~(memory_host_page-1));
        const auto ro=base+m.ro_offset,data=base+m.data_offset,end=base+m.size;
        if(address<text)return {base,text-base,4,3};
        if(address<text_end)return {text,text_end-text,3,5};
        if(address<ro)return {text_end,ro-text_end,4,3};
        if(address<data)return {ro,data-ro,3,1};
        return {data,end-data,4,3};
    }
    return {};
}
bool memory_borrowed(uintptr_t address,size_t size){
    for(const auto& m:mappings)if(m.state==0xb&&m.source&&memory_overlaps(address,size,m.source,m.size))return true;
    for(const auto& t:transfer_sources)if(memory_overlaps(address,size,t.base,t.size))return true;
    return false;
}
bool memory_source_available(uintptr_t address,size_t size){
    if(!size||size>UINTPTR_MAX-address||memory_borrowed(address,size))return false;
    const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    const auto end=address+size;auto cursor=address;
    while(cursor<end){
        auto next=cursor;
        if(memory_contains(heap,heap_size,cursor,1))next=heap+heap_size;
        for(const auto& m:mappings)if(m.state==5&&memory_contains(m.base,m.size,cursor,1))next=std::max(next,m.base+m.size);
        const auto image=memory_image_region(cursor);
        if(image.size&&image.state==4&&image.permission==3)next=std::max(next,image.base+image.size);
        if(next==cursor)return false;cursor=next;
    }
    return true;
}
bool memory_destination_free(uintptr_t address,size_t size){
    const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    if(memory_overlaps(address,size,heap,heap_size))return false;
    for(const auto& m:mappings)if(memory_overlaps(address,size,m.base,m.size))return false;
    return true;
}
uint64_t memory_permission_scans=0;
uint64_t memory_permission_epoch=1,memory_permission_hits=0;
struct PermissionEntry {uintptr_t address{};uint64_t epoch{};int permission{};};
std::array<PermissionEntry,64> memory_permission_cache{};
size_t memory_permission_next=0;
bool memory_permission_cache_enabled=true;
void memory_registry_changed(){
    if(++memory_permission_epoch==0){memory_permission_cache={};memory_permission_epoch=1;}
}
int memory_permission_uncached(uintptr_t address){
    ++memory_permission_scans;
    for(const auto& m:mappings)if(m.state==0xb&&m.source&&memory_contains(m.source,m.size,address,1))return PROT_NONE;
    for(const auto& t:transfer_sources)if(memory_contains(t.base,t.size,address,1))return t.permission;
    const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    if(memory_contains(heap,heap_size,address,1))return PROT_READ|PROT_WRITE;
    for(const auto& m:mappings)if(memory_contains(m.base,m.size,address,1))return PROT_READ|PROT_WRITE;
    if(const auto image=memory_image_region(address);image.size)return image.permission;
    return PROT_NONE;
}
// All registry access is serialized by mapping_mutex. Cache exact addresses,
// not host pages: 4 KiB guest subpages may have distinct permissions.
int memory_permission_at(uintptr_t address){
    // A field's first and last bytes are queried together. Direct mapping
    // aliases those neighboring addresses, so keep the small hot set fully
    // associative rather than repeatedly scanning the entire mapping registry.
    if(memory_permission_cache_enabled)for(const auto& slot:memory_permission_cache)
        if(slot.epoch==memory_permission_epoch&&slot.address==address){++memory_permission_hits;return slot.permission;}
    const int permission=memory_permission_uncached(address);
    if(memory_permission_cache_enabled){
        memory_permission_cache[memory_permission_next]={address,memory_permission_epoch,permission};
        memory_permission_next=(memory_permission_next+1)%memory_permission_cache.size();
    }
    return permission;
}
void memory_query_source(uintptr_t address,uint64_t& base,uint64_t& size,uint32_t& state,uint32_t& attribute,uint32_t& permission){
    auto end=base+size;
    auto borrowed=[&](uintptr_t start,size_t length,uint32_t allowed){
        if(memory_contains(start,length,address,1)){
            // Borrowing changes attributes and permission, not the original
            // memory type: static SDK stacks can reside in module CodeMutable.
            base=std::max(base,uint64_t(start));size=std::min(end,uint64_t(start+length))-base;
            attribute=1;permission=allowed;return true;
        }
        if(size){if(start+length<=address)base=std::max(base,uint64_t(start+length));else if(start>address)end=std::min(end,uint64_t(start));}
        return false;
    };
    for(const auto& m:mappings)if(m.state==0xb&&m.source&&borrowed(m.source,m.size,0))return;
    for(const auto& t:transfer_sources)if(borrowed(t.base,t.size,t.permission))return;
    if(size)size=end-base;
}
void memory_protect(uintptr_t address,size_t size,int permission){
    memory_registry_changed();
    if(!size)return;
    const auto end=address+size,first=address&~uintptr_t(memory_host_page-1),last=(end-1)&~uintptr_t(memory_host_page-1);
    auto boundary=[&](uintptr_t page){
        int combined=permission;
        for(auto p=page;p<page+memory_host_page;p+=4096)if(p<address||p>=end)combined|=memory_permission_at(p);
        if(mprotect(reinterpret_cast<void*>(page),memory_host_page,combined))fail("Guest memory boundary protection failed");
    };
    if(first==last){boundary(first);return;}
    const auto begin=address==first?first:first+memory_host_page;
    const auto finish=end==last+memory_host_page?end:last;
    if(begin<finish&&mprotect(reinterpret_cast<void*>(begin),finish-begin,permission))fail("Guest memory protection failed");
    if(address!=first)boundary(first);
    if(end!=last+memory_host_page)boundary(last);
}
void memory_discard(uintptr_t address,size_t size){
    memory_registry_changed();
    // Metadata already excludes this range. Replace full host pages with fresh
    // anonymous storage, also detaching a prior shared backing. A partial host
    // page is zeroed in place so its other live guest pages remain intact.
    const auto end=address+size,begin=(address+memory_host_page-1)&~uintptr_t(memory_host_page-1),finish=end&~uintptr_t(memory_host_page-1);
    auto partial=[&](uintptr_t p,size_t n){if(!n)return;memory_protect(p,n,PROT_READ|PROT_WRITE);memset(reinterpret_cast<void*>(p),0,n);memory_protect(p,n,PROT_NONE);};
    if(begin>=end){partial(address,size);return;}
    partial(address,begin-address);
    if(finish>begin&&mmap(reinterpret_cast<void*>(begin),finish-begin,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED)fail("Guest memory release failed");
    partial(finish,end-finish);
}
uint64_t memory_validate(uintptr_t address,size_t size,uint64_t overflow_result){
    if(address&4095)return memory_invalid_address;
    if(!size||(size&4095))return memory_invalid_size;
    if(size>UINTPTR_MAX-address)return overflow_result;
    return 0;
}
uint64_t memory_physical(bool map,uintptr_t address,size_t size){
    if(auto result=memory_validate(address,size,memory_invalid_region))return result;
    const auto base=reinterpret_cast<uintptr_t>(address_space)+alias_offset;
    if(!memory_contains(base,alias_size,address,size))return memory_invalid_region;
    if(!map&&memory_borrowed(address,size))return memory_invalid_current;
    // Calculate the complete next registry before changing permissions/data.
    // Free ranges can be crossed; only committed normal pages are released.
    auto next=mappings;
    if(map){
        std::vector<std::pair<uintptr_t,uintptr_t>> occupied;
        for(const auto& m:mappings)if(memory_overlaps(address,size,m.base,m.size))occupied.push_back({std::max(address,m.base),std::min(address+size,m.base+m.size)});
        std::sort(occupied.begin(),occupied.end());
        std::vector<Mapping> holes;auto cursor=address;
        for(auto [lo,hi]:occupied){if(lo>cursor)holes.push_back({cursor,lo-cursor,5,0});cursor=std::max(cursor,hi);}
        if(cursor<address+size)holes.push_back({cursor,address+size-cursor,5,0});
        next.insert(next.end(),holes.begin(),holes.end());mappings.swap(next);
        for(const auto& hole:holes)memory_protect(hole.base,hole.size,PROT_READ|PROT_WRITE);
    }else{
        next.clear();
        for(const auto& m:mappings){
            if(!memory_overlaps(address,size,m.base,m.size)){next.push_back(m);continue;}
            if(m.state!=5||m.source)return memory_invalid_current;
            if(m.base<address)next.push_back({m.base,address-m.base,m.state,0});
            if(m.base+m.size>address+size)next.push_back({address+size,m.base+m.size-address-size,m.state,0});
        }
        mappings.swap(next);memory_discard(address,size);
    }
    return 0;
}
uint64_t memory_stack(bool map,uintptr_t destination,uintptr_t source,size_t size){
    if((destination|source)&4095)return memory_invalid_address;
    if(!size||(size&4095))return memory_invalid_size;
    if(size>UINTPTR_MAX-destination||size>UINTPTR_MAX-source)return memory_invalid_current;
    const auto base=reinterpret_cast<uintptr_t>(address_space);
    if(!memory_contains(base+stack_offset,stack_capacity,destination,size))return memory_invalid_region;
    if(map){
        if(!memory_source_available(source,size)||!memory_destination_free(destination,size)){
            log("MAP_STACK_REJECT source=%llx destination=%llx size=%zx borrowed=%d available=%d destination_free=%d",(unsigned long long)source,(unsigned long long)destination,size,memory_borrowed(source,size),memory_source_available(source,size),memory_destination_free(destination,size));
            return memory_invalid_current;
        }
        mappings.reserve(mappings.size()+1);
        memory_protect(destination,size,PROT_READ|PROT_WRITE);
        memcpy(reinterpret_cast<void*>(destination),reinterpret_cast<void*>(source),size);
        mappings.push_back({destination,size,0xb,source});memory_protect(source,size,PROT_NONE);
    }else{
        auto it=std::find_if(mappings.begin(),mappings.end(),[&](const Mapping& m){return m.state==0xb&&m.base==destination&&m.size==size&&m.source==source;});
        if(it==mappings.end())return memory_invalid_current;
        memory_protect(source,size,PROT_READ|PROT_WRITE);
        memcpy(reinterpret_cast<void*>(source),reinterpret_cast<void*>(destination),size);
        mappings.erase(it);memory_discard(destination,size);
    }
    return 0;
}
uint64_t memory_resize_heap(size_t size){
    if(size&((2*1024*1024)-1))return memory_invalid_size;
    if(size>heap_capacity)return memory_invalid_current;
    constexpr uint64_t pool=0xcd500000;
    if(image_size>pool||size>pool-image_size)return 0x10801;
    const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    if(size<heap_size&&memory_borrowed(heap+size,heap_size-size))return memory_invalid_current;
    if(size>heap_size&&!memory_destination_free(heap+heap_size,size-heap_size))return memory_invalid_current;
    const auto old=heap_size;heap_size=size;
    if(size>old)memory_protect(heap+old,size-old,PROT_READ|PROT_WRITE);
    else if(size<old)memory_discard(heap+size,old-size);
    return 0;
}
