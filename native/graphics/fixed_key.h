#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// Field-wise keys have no pointer or structure padding. Equality always checks
// every byte; the hash only selects a bucket, never establishes identity.
template<size_t Capacity> struct FixedKey {
    std::array<uint8_t,Capacity> bytes{};
    size_t used=0;
    template<class T> void add(const T& value){
        if(sizeof(T)>Capacity-used)std::abort();
        std::memcpy(bytes.data()+used,&value,sizeof(T));used+=sizeof(T);
    }
    bool operator==(const FixedKey& other)const{return used==other.used&&bytes==other.bytes;}
    struct Hash {
        size_t operator()(const FixedKey& key)const{
            uint64_t hash=key.used;
            for(size_t at=0;at<Capacity;at+=8){
                uint64_t word=0;std::memcpy(&word,key.bytes.data()+at,std::min(size_t(8),Capacity-at));
                hash^=word+0x9e3779b97f4a7c15ull+(hash<<6)+(hash>>2);
            }
            return hash;
        }
    };
};
