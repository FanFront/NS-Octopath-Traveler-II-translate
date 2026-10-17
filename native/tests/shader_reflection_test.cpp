#include "../graphics/shader_reflection.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Specify shader cache directory");size_t shaders=0,resources=0;
    for(auto& entry:std::filesystem::directory_iterator(argv[1])){
        const auto name=entry.path().filename().string();if(!name.ends_with(".res.json"))continue;
        nlohmann::json json;std::ifstream(entry.path())>>json;ShaderReflection r(json);
        if(r.key+".res.json"!=name)throw std::runtime_error("Reflection filename/key mismatch");
        resources+=r.cbufs.size()+r.sbufs.size()+r.textures.size()+r.images.size();++shaders;
    }
    if(!shaders)throw std::runtime_error("No shader metadata found");
    printf("PASS %zu shader reflections, %zu typed resource bindings\n",shaders,resources);return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL reflection: %s\n",e.what());return 1;}}
