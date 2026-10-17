#include "../graphics/epoch_cache.h"
#include <cstdio>
#include <stdexcept>
int main(){
    EpochCache<4,uint64_t> cache;
    auto require=[](bool ok){if(!ok)throw std::runtime_error("recording snapshot epoch/hash contract");};
    // Exercise resize, probe collisions, exact keys, replacing a key, and
    // clearing/reusing a full recording's storage with different values.
    for(unsigned pass=0;pass<8;++pass){
        auto capacity=cache.capacity();
        for(uint64_t i=0;i<10000;++i){
            EpochCache<4,uint64_t>::Key key{i*4096,64,i%7,65536};
            require(!cache.find(key));cache.insert(key,i+pass*10000);
        }
        if(pass)require(cache.capacity()==capacity);
        for(uint64_t i=0;i<10000;++i){
            EpochCache<4,uint64_t>::Key key{i*4096,64,i%7,65536};
            auto* value=cache.find(key);require(value&&*value==i+pass*10000);
            auto changed=key;changed[1]=65;require(!cache.find(changed));
            changed=key;changed[3]=16;require(!cache.find(changed));
            cache.insert(key,~i);require(*cache.find(key)==~i);
        }
        cache.clear();require(!cache.find({0,64,0,65536}));
    }
    puts("PASS recording snapshots: exact keys, collisions, growth, replacement, epochs, retained capacity");
}
