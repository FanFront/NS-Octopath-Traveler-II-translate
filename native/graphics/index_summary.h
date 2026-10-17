#pragma once
#include "epoch_cache.h"
#include "nvn_dirty.h"
#include <algorithm>
#include <cstring>

// The submit lock serializes lookups and pool lifetimes. Callers must resolve
// pending GPU writes before reading indices, just as for an uncached scan.
class IndexSummaries {
    struct Summary {uint64_t stamp,seen;uint32_t highest;};
    EpochCache<3,Summary> entries;
public:
    bool enabled=true;
    uint64_t hits=0,scans=0,scanned_bytes=0;
    void clear(){entries.clear();}
    size_t size()const{return entries.size();}
    // type has already been validated: 0=u8, 1=u16, 2=u32.
    uint32_t maximum(const void* data,size_t count,unsigned type){
        if(!count)return 0;
        const size_t width=size_t(1)<<type,bytes=count*width;
        const EpochCache<3,Summary>::Key key{uintptr_t(data),count,type};
        auto now=nvn_dirty_now();
        if(enabled)if(auto* cached=entries.find(key)){
            if(now==cached->seen||!nvn_dirty_since(data,bytes,cached->stamp)){
                cached->seen=now;++hits;return cached->highest;
            }
        }
        // Untracked allocations are always scanned. Protecting a tracked range
        // before the read ensures later guest/host writes invalidate this fact.
        bool tracked=enabled&&nvn_dirty_protect(data,bytes);
        const auto stamp=nvn_dirty_now();
        uint32_t highest=0;auto* p=static_cast<const uint8_t*>(data);
        for(size_t i=0;i<count;i++){
            uint32_t value=0;
            if(type==0)value=p[i];
            else if(type==1){uint16_t v;memcpy(&v,p+i*2,2);value=v;}
            else memcpy(&value,p+i*4,4);
            highest=std::max(highest,value);
        }
        ++scans;scanned_bytes+=bytes;
        if(tracked){if(entries.size()>=16384)entries.clear();entries.insert(key,{stamp,stamp,highest});}
        return highest;
    }
};
