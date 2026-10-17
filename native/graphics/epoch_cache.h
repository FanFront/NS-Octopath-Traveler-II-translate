#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

// Recording-local snapshots have externally owned GPU storage. Clear invalidates
// their lookup epoch without allocating/freeing a tree node for every upload.
// Values must not own resources; their storage lifetime is managed by GPU fences.
template<size_t Words,class Value> class EpochCache {
    static_assert(std::is_trivially_copyable_v<Value>);
public:
    using Key=std::array<uint64_t,Words>;
private:
    struct Entry {Key key{};Value value{};uint64_t epoch{};};
    std::vector<Entry> entries;
    uint64_t epoch=1;
    size_t count=0;
    static size_t hash(const Key& key){
        uint64_t h=0;
        for(auto x:key){x^=x>>30;x*=0xbf58476d1ce4e5b9ULL;x^=x>>27;x*=0x94d049bb133111ebULL;x^=x>>31;h^=x+0x9e3779b97f4a7c15ULL+(h<<6)+(h>>2);}
        return h;
    }
    Entry& slot(const Key& key){
        auto i=hash(key)&(entries.size()-1);
        while(entries[i].epoch==epoch&&entries[i].key!=key)i=(i+1)&(entries.size()-1);
        return entries[i];
    }
    void grow(){
        auto old=std::move(entries);entries.resize(old.empty()?256:old.size()*2);count=0;
        for(const auto& e:old)if(e.epoch==epoch)insert(e.key,e.value);
    }
public:
    Value* find(const Key& key){
        if(entries.empty())return nullptr;
        auto& e=slot(key);return e.epoch==epoch?&e.value:nullptr;
    }
    void insert(const Key& key,const Value& value){
        if(entries.empty()||(count+1)*4>entries.size()*3)grow();
        auto& e=slot(key);if(e.epoch!=epoch)++count;e={key,value,epoch};
    }
    void clear(){
        count=0;if(++epoch==0){for(auto& e:entries)e.epoch=0;epoch=1;}
    }
    size_t capacity()const{return entries.size();}
    size_t size()const{return count;}
};
