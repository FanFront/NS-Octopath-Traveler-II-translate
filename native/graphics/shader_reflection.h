#pragma once
#include "../third_party/json.hpp"
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
struct ShaderResource {
    uint32_t binding{},handle{},type{},flags{},array=1,cb_offset{};
    int slot{},cb_slot{},cbuf_slot=-1;
    bool separate=false;
};
struct ShaderReflection {
    std::string key;
    std::vector<ShaderResource> cbufs,sbufs,textures,images;
    bool side_effects=false;
    uint32_t bank_masks[4]{};
    explicit ShaderReflection(const nlohmann::json& json){
        key=json.at("key").get<std::string>();if(key.size()!=16)throw std::runtime_error("Invalid shader reflection key");
        std::set<uint32_t> bindings;
        auto read=[&](const char* group,std::vector<ShaderResource>& out,int kind){
            const auto& list=json.at(group);if(!list.is_array())throw std::runtime_error("Shader resource list must be an array");
            for(const auto& item:list){ShaderResource r;r.binding=item.at("binding");
                if(!r.binding||!bindings.insert(r.binding).second)throw std::runtime_error("Duplicate/reserved shader binding");
                if(kind==0){r.slot=item.at("slot");if(r.slot<0||r.slot>=19)throw std::runtime_error("Invalid constant bank");}
                if(kind==1){r.cb_slot=item.at("cb_slot");r.cb_offset=item.at("cb_offset");r.flags=item.value("flags",1u);
                    if(r.cb_slot<0||r.cb_slot>=19||r.cb_offset>16381)throw std::runtime_error("Invalid storage bank reference");side_effects|=(r.flags&1)!=0;}
                if(kind>=2){r.handle=item.at("handle");r.cbuf_slot=item.at("cbuf_slot");r.type=item.at("type");r.array=item.at("array");r.separate=item.at("separate");r.flags=item.value("flags",kind==3?4u:0u);
                    if(!r.array||(r.handle>>28)>4)throw std::runtime_error("Invalid image resource");if(kind==3)side_effects|=(r.flags&4)!=0;}
                out.push_back(r);
            }
        };
        read("cbufs",cbufs,0);read("sbufs",sbufs,1);read("textures",textures,2);read("images",images,3);
        for(const auto& r:cbufs)bank_masks[0]|=1u<<r.slot;
        for(const auto& r:sbufs)bank_masks[1]|=1u<<r.cb_slot;
        for(int group=2;group<4;++group)for(const auto& r:group==2?textures:images){
            const auto type=r.handle>>28;if(type==4)continue;
            const int cb=r.cbuf_slot<0?2:r.cbuf_slot&65535;
            const int sb=r.cbuf_slot<0?cb:(uint32_t(r.cbuf_slot)>>16?int(uint32_t(r.cbuf_slot)>>16)-1:cb);
            if(cb<0||cb>=19||(type&&type!=3&&(sb<0||sb>=19)))throw std::runtime_error("Invalid resource handle bank");
            bank_masks[group]|=1u<<cb;if(type&&type!=3)bank_masks[group]|=1u<<sb;
        }
    }
};
