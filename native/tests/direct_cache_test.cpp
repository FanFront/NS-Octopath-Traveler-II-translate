#include "../graphics/direct_cache.h"
#include <stdexcept>
#include <cstdio>
int main(){
    auto require=[](bool ok){if(!ok)throw std::runtime_error("direct cache key/epoch contract");};
    // Capacity 1 forces every key to collide, including equal addresses with
    // different size, descriptor type and padding requirements.
    DirectCache<4,uint64_t,1> cache;
    for(unsigned epoch=0;epoch<8;epoch++)for(unsigned field=0;field<4;field++){
        const DirectCache<4,uint64_t,1>::Key a{0x10000,64,16,65536};auto b=a;b[field]++;
        cache.insert(a,17);require(cache.find(a)&&*cache.find(a)==17&&!cache.find(b));
        cache.insert(b,23);require(!cache.find(a)&&cache.find(b)&&*cache.find(b)==23);
        cache.clear();require(!cache.find(a)&&!cache.find(b));
    }
    puts("PASS direct cache: full keys, forced collision, replacement, invalidation");
}
