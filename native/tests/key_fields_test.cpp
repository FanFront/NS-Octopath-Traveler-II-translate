#include "../graphics/key_fields.h"
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>

template<class... T> void check(const T&... fields){
    std::string old="prefix",packed=old;
    (old.append(reinterpret_cast<const char*>(&fields),sizeof(fields)),...);
    append_key_fields(packed,fields...);
    if(old!=packed)throw std::runtime_error("persisted descriptor key bytes changed");
}
int main(){
    std::mt19937_64 random(32749);
    for(unsigned i=0;i<10000;++i){
        uint32_t binding=random(),type=random(),kind=random(),count=random(),stage=random();
        uint64_t buffer=random(),offset=random(),range=random(),image=random(),sampler=random();
        check(binding,type,count,stage); // Layout: no pointer/padding bytes.
        check(binding,type,kind,buffer,offset,range);
        check(binding,type,kind,image,sampler,stage);
        check(binding,type,kind,buffer);
    }
    puts("PASS descriptor/layout keys remain byte identical across all payload kinds");
}
