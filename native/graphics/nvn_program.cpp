#include "nvn_program.h"
#include "../thread_services.h"
#include <map>
#include <mutex>
namespace {
std::mutex mutex;
struct Entry {std::shared_ptr<const NvnProgramVersion> owner;std::weak_ptr<const NvnProgramVersion> version;};
std::map<uint64_t,Entry> versions;
}
NvnProgramVersion::NvnProgramVersion(const prog_t& p):value(p){
    if(p.nstage>6)ot2_host_die("Program has too many stages");
    for(unsigned i=0;i<p.nstage;i++){
        if(!p.sd[i].control||!p.sd[i].data)ot2_host_die("Missing shader control/code");
        memcpy(controls[i].data(),p.sd[i].control,controls[i].size());uint32_t magic,size,offset,stage;
        memcpy(&magic,controls[i].data(),4);memcpy(&offset,controls[i].data()+0x700,4);memcpy(&size,controls[i].data()+0x704,4);memcpy(&stage,controls[i].data()+0x714,4);
        if(magic!=0x98761234||size<8||size>8*1024*1024||offset>size||stage>=6||stages[stage]!=-1)ot2_host_die("Invalid shader program metadata");
        auto* data=reinterpret_cast<const uint8_t*>(p.sd[i].data);code[i].assign(data,data+size);
        value.sd[i].control=controls[i].data();value.sd[i].data=reinterpret_cast<uint64_t>(code[i].data());stages[stage]=i;
        uint64_t hash=14695981039346656037ULL;for(auto byte:controls[i])hash=(hash^byte)*1099511628211ULL;
        char key[17];snprintf(key,sizeof(key),"%016llx",(unsigned long long)hash);keys[i]=key;
    }
}
extern "C" void nvn_program_publish(prog_t* p){
    auto version=std::make_shared<NvnProgramVersion>(*p);*p=version->value;
    std::lock_guard lock(mutex);versions[p->serial]={version,version};
    std::erase_if(versions,[](const auto& e){return !e.second.owner&&e.second.version.expired();});
}
extern "C" void nvn_program_release(uint64_t serial){std::lock_guard lock(mutex);auto i=versions.find(serial);if(i!=versions.end()){i->second.owner.reset();if(i->second.version.expired())versions.erase(i);}}
std::shared_ptr<const NvnProgramVersion> nvn_program_get(uint64_t serial){std::lock_guard lock(mutex);auto i=versions.find(serial);if(i==versions.end())ot2_host_die("Program version is no longer owned");auto p=i->second.version.lock();if(!p)ot2_host_die("Program version expired");return p;}
