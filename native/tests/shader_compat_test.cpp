#include "../graphics/shader_compat.h"
#include "../graphics/clear_shaders.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){try{
    if(argc<2||argc>3)throw std::runtime_error("output directory [game shader corpus] required");
    std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    auto save=[&](const char* name,const auto& words){std::ofstream f(root/name,std::ios::binary);f.write(reinterpret_cast<const char*>(words.data()),words.size()*4);if(!f)throw std::runtime_error("write failed");};
    const uint32_t bindings[]={2,4,5,6,7,8,9,10,11,12,13,14,15};
    auto nulls=shader_null_descriptors(null_resource_probe_comp,bindings);
    if(shader_null_descriptors(nulls,bindings)!=nulls)throw std::runtime_error("null rewrite not idempotent");
    save("samples.spv",shader_count_samples(layered_probe_frag));save("null.spv",nulls);save("depth.spv",shader_readonly_depth_bounds(state_probe_frag));save("depth-sampled.spv",shader_readonly_depth_bounds(attachment_probe_frag));
    const ShaderIntegerImage integer_image{1,0,33};auto integer=shader_integer_images(format_texel_probe_comp,{&integer_image,1});save("integer-texel.spv",integer);
    if(shader_integer_images(integer,{&integer_image,1})!=integer)throw std::runtime_error("integer texel rewrite not idempotent");
    save("integer-storage.spv",shader_integer_images(format_storage_probe_comp,{&integer_image,1}));
    auto stencil=shader_integer_images(lighting_mask_probe_frag,{&integer_image,1});
    const ShaderIntegerOutput integer_output{0,0};auto mask=shader_integer_outputs(stencil,{&integer_output,1});save("integer-mask.spv",mask);
    if(shader_integer_outputs(mask,{&integer_output,1})!=mask)throw std::runtime_error("integer output rewrite not idempotent");
    save("integer-2d.spv",shader_integer_images(integer_2d_probe_comp,{&integer_image,1}));
    bool rejected=false;try{shader_readonly_depth_bounds(state_probe_vert);}catch(const std::runtime_error&){rejected=true;}
    if(!rejected)throw std::runtime_error("non-fragment accepted for depth fallback");
    rejected=false;try{shader_count_samples(state_probe_vert);}catch(const std::runtime_error&){rejected=true;}
    if(!rejected)throw std::runtime_error("non-fragment accepted for sample counting");
    std::vector<uint32_t> early(std::begin(sample_count_probe_frag),std::end(sample_count_probe_frag));
    for(size_t at=5;at<early.size();at+=early[at]>>16)if((early[at]&65535)==16){const uint32_t instruction[]={(3u<<16)|16u,early[at+1],9};early.insert(early.begin()+at,instruction,instruction+3);break;}
    rejected=false;try{shader_count_samples(early);}catch(const std::runtime_error&){rejected=true;}
    if(!rejected)throw std::runtime_error("early fragment tests accepted for late sample counting");
    if(argc==3)for(const auto& entry:std::filesystem::directory_iterator(argv[2]))if(entry.path().extension()==".spv"){
        std::ifstream file(entry.path(),std::ios::binary|std::ios::ate);auto bytes=file.tellg();if(bytes<20||size_t(bytes)%4)throw std::runtime_error("invalid corpus file");
        std::vector<uint32_t> words(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),bytes);
        auto portable=shader_without_unused_interlock(words);auto stem=entry.path().stem().string();save((stem+"-portable.spv").c_str(),portable);
        std::vector<uint32_t> all_bindings;bool fragment=false;
        for(size_t at=5;at<portable.size();){auto count=portable[at]>>16,op=portable[at]&65535;if(!count||at+count>portable.size())throw std::runtime_error("invalid corpus instruction");
            if(op==71&&count==4&&portable[at+2]==33)all_bindings.push_back(portable[at+3]);
            if(op==15&&count>3&&portable[at+1]==4)fragment=true;at+=count;
        }
        auto rewritten=shader_null_descriptors(portable,all_bindings);save((stem+"-null.spv").c_str(),rewritten);
        if(fragment){save((stem+"-samples.spv").c_str(),shader_count_samples(portable));save((stem+"-depth.spv").c_str(),shader_readonly_depth_bounds(portable));save((stem+"-null-depth.spv").c_str(),shader_readonly_depth_bounds(rewritten));}
    }
    std::cout<<"PASS shader rewrite fixture generation, idempotence and stage rejection; run spirv-val on outputs\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
