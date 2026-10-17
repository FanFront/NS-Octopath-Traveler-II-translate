#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// A bounded front cache. Collisions replace a slot; full-key equality and the
// caller's lifetime epoch are mandatory. It never owns the referenced storage.
template<size_t Words,class Value,size_t Capacity=256> class DirectCache {
    static_assert(Capacity && !(Capacity&(Capacity-1)));
    static_assert(std::is_trivially_copyable_v<Value>);
public:
    using Key=std::array<uint64_t,Words>;
private:
    struct Entry {Key key{};Value value{};uint64_t epoch{};};
    std::array<Entry,Capacity> entries{};
    uint64_t epoch=1;
    size_t bucket(const Key& key)const{
        uint64_t h=0;for(auto x:key)h=(h<<5)^(h>>2)^x^(x>>11)^(x>>23);
        return h&(Capacity-1);
    }
public:
    const Value* find(const Key& key)const{
        const auto& e=entries[bucket(key)];return e.epoch==epoch&&e.key==key?&e.value:nullptr;
    }
    void insert(const Key& key,Value value){entries[bucket(key)]={key,value,epoch};}
    void clear(){if(++epoch==0){for(auto& e:entries)e.epoch=0;epoch=1;}}
};
