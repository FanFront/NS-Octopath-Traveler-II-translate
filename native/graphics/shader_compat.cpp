#include "shader_compat.h"
#define SPV_ENABLE_UTILITY_CODE
#include "../third_party/spirv/spirv.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <tuple>

namespace {
struct Instruction {
    spv::Op op;
    std::vector<uint32_t> w;
    uint32_t result{},type{};
};
std::vector<Instruction> parse(std::span<const uint32_t> words){
    if(words.size()<5||words[0]!=spv::MagicNumber||words[3]>1048576)throw std::runtime_error("Invalid shader compatibility input");
    std::vector<Instruction> code;
    for(size_t at=5;at<words.size();){
        auto count=words[at]>>16;if(!count||count>words.size()-at)throw std::runtime_error("Invalid shader compatibility instruction");
        Instruction i{spv::Op(words[at]&65535),{words.begin()+at,words.begin()+at+count}};
        bool result,type;spv::HasResultAndType(i.op,&result,&type);
        if(result){if(count<(type?3u:2u))throw std::runtime_error("Invalid shader result");i.result=i.w[type?2:1];if(i.result>=words[3])throw std::runtime_error("Invalid shader id");}
        if(type)i.type=i.w[1];code.push_back(std::move(i));at+=count;
    }
    return code;
}
void emit(std::vector<uint32_t>& out,spv::Op op,std::initializer_list<uint32_t> operands){
    out.push_back(uint32_t(operands.size()+1)<<16|uint32_t(op));out.insert(out.end(),operands);
}
}

std::vector<uint32_t> shader_integer_images(std::span<const uint32_t> words,std::span<const ShaderIntegerImage> images){
    if(images.empty())return {words.begin(),words.end()};
    auto code=parse(words);uint32_t next=words[3];
    std::map<uint32_t,const Instruction*> defs;std::map<uint32_t,uint32_t> sets,bindings;
    for(const auto& i:code){if(i.result)defs[i.result]=&i;if(i.op==spv::OpDecorate&&i.w.size()==4){if(i.w[2]==spv::DecorationDescriptorSet)sets[i.w[1]]=i.w[3];if(i.w[2]==spv::DecorationBinding)bindings[i.w[1]]=i.w[3];}}
    std::vector<uint32_t> declarations;
    std::map<std::vector<uint32_t>,uint32_t> generated;
    auto type=[&](spv::Op op,std::vector<uint32_t> operands){
        std::vector<uint32_t> key{uint32_t(op)};key.insert(key.end(),operands.begin(),operands.end());
        if(auto f=generated.find(key);f!=generated.end())return f->second;
        for(const auto& i:code)if(i.op==op&&i.w.size()==operands.size()+2&&std::equal(operands.begin(),operands.end(),i.w.begin()+2))return i.result;
        auto id=next++;declarations.push_back(uint32_t(operands.size()+2)<<16|uint32_t(op));declarations.push_back(id);declarations.insert(declarations.end(),operands.begin(),operands.end());generated.emplace(std::move(key),id);return id;
    };
    std::map<std::tuple<uint32_t,uint32_t,uint32_t>,uint32_t> changed_types;
    std::function<uint32_t(uint32_t,const ShaderIntegerImage&)> specialize=[&](uint32_t old,const auto& f)->uint32_t{
        auto key=std::tuple{old,f.signedness,f.image_format};if(auto found=changed_types.find(key);found!=changed_types.end())return found->second;
        const auto& i=*defs.at(old);auto operands=std::vector<uint32_t>(i.w.begin()+2,i.w.end());
        if(i.op==spv::OpTypeImage){if(i.w.size()<9||i.w[4]==1)throw std::runtime_error("Integer image specialization cannot use depth comparisons");
            operands[0]=type(spv::OpTypeInt,{32,f.signedness});if(i.w[7]==2)operands[6]=f.image_format;
        }else if(i.op==spv::OpTypeSampledImage||i.op==spv::OpTypeArray)operands[0]=specialize(operands[0],f);
        else if(i.op==spv::OpTypePointer)operands[1]=specialize(operands[1],f);
        else throw std::runtime_error("Unsupported integer image type chain");
        auto result=type(i.op,std::move(operands));changed_types[key]=result;return result;
    };
    std::map<uint32_t,ShaderIntegerImage> provenance;std::map<uint32_t,uint32_t> replacement_types;
    for(const auto& i:code)if(i.op==spv::OpVariable&&sets.contains(i.result)&&sets.at(i.result)==0&&bindings.contains(i.result))for(const auto& f:images)if(f.binding==bindings.at(i.result)){
        if(f.signedness>1||!f.image_format)throw std::runtime_error("Invalid integer image format");
        auto changed=specialize(i.type,f);if(changed!=i.type){provenance[i.result]=f;replacement_types[i.result]=changed;}break;
    }
    if(provenance.empty())return {words.begin(),words.end()};
    for(unsigned pass=0;pass<16;pass++){bool changed=false;
        for(const auto& i:code){if(!i.result||provenance.contains(i.result))continue;size_t source=0;
            switch(i.op){case spv::OpLoad:case spv::OpCopyObject:case spv::OpAccessChain:case spv::OpInBoundsAccessChain:case spv::OpImage:case spv::OpSampledImage:source=3;break;default:break;}
            if(source&&i.w.size()>source&&provenance.contains(i.w[source])){auto f=provenance.at(i.w[source]);provenance[i.result]=f;replacement_types[i.result]=specialize(i.type,f);changed=true;}
        }if(!changed)break;if(pass==15)throw std::runtime_error("Integer image provenance did not converge");
    }
    auto numeric_type=[&](uint32_t old,const ShaderIntegerImage& f){auto scalar=type(spv::OpTypeInt,{32,f.signedness});const auto& t=*defs.at(old);
        if(t.op==spv::OpTypeVector)return type(spv::OpTypeVector,{scalar,t.w[3]});
        if((t.op!=spv::OpTypeInt&&t.op!=spv::OpTypeFloat)||t.w[2]!=32)throw std::runtime_error("Integer texture result must have 32-bit components");return scalar;
    };
    std::vector<uint32_t> globals,body;bool functions=false;
    for(const auto& i:code){functions|=i.op==spv::OpFunction;
        if(!functions&&i.op==spv::OpVariable){auto w=i.w;if(replacement_types.contains(i.result))w[1]=replacement_types.at(i.result);globals.insert(globals.end(),w.begin(),w.end());continue;}
        if(!functions)continue;
        if(replacement_types.contains(i.result)){auto w=i.w;w[1]=replacement_types.at(i.result);body.insert(body.end(),w.begin(),w.end());continue;}
        bool read=i.op==spv::OpImageFetch||i.op==spv::OpImageRead||i.op==spv::OpImageSampleImplicitLod||i.op==spv::OpImageSampleExplicitLod||i.op==spv::OpImageGather;
        if(read&&provenance.contains(i.w[3])){auto target=numeric_type(i.type,provenance.at(i.w[3]));auto w=i.w;
            if(target!=i.type){w[1]=target;w[2]=next++;body.insert(body.end(),w.begin(),w.end());emit(body,spv::OpBitcast,{i.type,i.result,w[2]});}
            else body.insert(body.end(),w.begin(),w.end());continue;
        }
        if(i.op==spv::OpImageWrite&&provenance.contains(i.w[1])){auto value=i.w[3];auto old=defs.at(value)->type;auto target=numeric_type(old,provenance.at(i.w[1]));auto w=i.w;
            if(target!=old){auto cast=next++;emit(body,spv::OpBitcast,{target,cast,value});w[3]=cast;}body.insert(body.end(),w.begin(),w.end());continue;
        }
        // Reject depth comparisons, projected/sparse sampling and opaque
        // values crossing control-flow/function boundaries.
        if(i.op==spv::OpPhi||i.op==spv::OpSelect||i.op==spv::OpFunctionCall||i.op==spv::OpImageTexelPointer||
           (i.op>=spv::OpImageSampleImplicitLod&&i.op<=spv::OpImageDrefGather)||
           (i.op>=spv::OpImageSparseSampleImplicitLod&&i.op<=spv::OpImageSparseDrefGather)||i.op==spv::OpImageSparseRead){
            for(size_t n=i.type?3:1;n<i.w.size();n++)if(provenance.contains(i.w[n]))throw std::runtime_error("Unsupported integer texel image use");
        }
        body.insert(body.end(),i.w.begin(),i.w.end());
    }
    std::vector<uint32_t> out(words.begin(),words.begin()+5);out[3]=next;
    for(const auto& i:code){if(i.op==spv::OpFunction)break;if(i.op!=spv::OpVariable)out.insert(out.end(),i.w.begin(),i.w.end());}
    out.insert(out.end(),declarations.begin(),declarations.end());out.insert(out.end(),globals.begin(),globals.end());out.insert(out.end(),body.begin(),body.end());return out;
}

std::vector<uint32_t> shader_integer_outputs(std::span<const uint32_t> words,std::span<const ShaderIntegerOutput> outputs){
    if(outputs.empty())return {words.begin(),words.end()};
    auto code=parse(words);uint32_t next=words[3];bool fragment=false;
    std::map<uint32_t,const Instruction*> defs;std::map<uint32_t,uint32_t> locations;
    for(const auto& i:code){if(i.result)defs[i.result]=&i;if(i.op==spv::OpEntryPoint&&i.w[1]==spv::ExecutionModelFragment)fragment=true;
        if(i.op==spv::OpDecorate&&i.w.size()==4&&i.w[2]==spv::DecorationLocation)locations[i.w[1]]=i.w[3];}
    if(!fragment)throw std::runtime_error("Integer outputs require a fragment shader");
    std::vector<uint32_t> declarations;std::map<std::vector<uint32_t>,uint32_t> generated;
    auto type=[&](spv::Op op,std::vector<uint32_t> operands){
        std::vector<uint32_t> key{uint32_t(op)};key.insert(key.end(),operands.begin(),operands.end());
        if(auto f=generated.find(key);f!=generated.end())return f->second;
        for(const auto& i:code)if(i.op==op&&i.w.size()==operands.size()+2&&std::equal(operands.begin(),operands.end(),i.w.begin()+2))return i.result;
        auto id=next++;declarations.push_back(uint32_t(operands.size()+2)<<16|uint32_t(op));declarations.push_back(id);declarations.insert(declarations.end(),operands.begin(),operands.end());generated.emplace(std::move(key),id);return id;
    };
    std::function<uint32_t(uint32_t,uint32_t)> numeric=[&](uint32_t old,uint32_t sign)->uint32_t{
        auto& t=*defs.at(old);
        if(t.op==spv::OpTypePointer){if(t.w[2]!=spv::StorageClassOutput)throw std::runtime_error("Integer output pointer storage class");return type(t.op,{t.w[2],numeric(t.w[3],sign)});}
        if(t.op==spv::OpTypeVector)return type(t.op,{numeric(t.w[2],sign),t.w[3]});
        if((t.op!=spv::OpTypeInt&&t.op!=spv::OpTypeFloat)||t.w[2]!=32)throw std::runtime_error("Integer output requires 32-bit scalar/vector");
        return type(spv::OpTypeInt,{32,sign});
    };
    std::map<uint32_t,uint32_t> signs,replacements;
    for(const auto& i:code)if(i.op==spv::OpVariable&&i.w[3]==spv::StorageClassOutput&&locations.contains(i.result))for(auto f:outputs)if(f.location==locations.at(i.result)){
        if(f.signedness>1)throw std::runtime_error("Invalid integer output signedness");auto t=numeric(i.type,f.signedness);
        if(t!=i.type){signs[i.result]=f.signedness;replacements[i.result]=t;}break;
    }
    if(signs.empty())return {words.begin(),words.end()};
    for(unsigned pass=0;pass<16;pass++){bool changed=false;for(const auto& i:code)if(i.result&&!signs.contains(i.result)&&(i.op==spv::OpAccessChain||i.op==spv::OpInBoundsAccessChain||i.op==spv::OpCopyObject)&&signs.contains(i.w[3])){
        signs[i.result]=signs.at(i.w[3]);replacements[i.result]=numeric(i.type,signs.at(i.result));changed=true;
    }if(!changed)break;if(pass==15)throw std::runtime_error("Integer output provenance did not converge");}
    std::vector<uint32_t> globals,body;bool functions=false;
    for(const auto& i:code){functions|=i.op==spv::OpFunction;
        if(!functions&&i.op==spv::OpVariable){auto w=i.w;if(replacements.contains(i.result)){if(w.size()!=4)throw std::runtime_error("Initialized integer output unsupported");w[1]=replacements.at(i.result);}globals.insert(globals.end(),w.begin(),w.end());continue;}
        if(!functions)continue;
        if(replacements.contains(i.result)){auto w=i.w;w[1]=replacements.at(i.result);body.insert(body.end(),w.begin(),w.end());continue;}
        if(i.op==spv::OpStore&&signs.contains(i.w[1])){auto w=i.w;auto old=defs.at(w[2])->type;auto target=numeric(old,signs.at(w[1]));
            if(target!=old){auto cast=next++;emit(body,spv::OpBitcast,{target,cast,w[2]});w[2]=cast;}body.insert(body.end(),w.begin(),w.end());continue;}
        if(i.op==spv::OpLoad&&signs.contains(i.w[3])){auto w=i.w;auto target=numeric(i.type,signs.at(w[3]));
            if(target!=i.type){w[1]=target;w[2]=next++;body.insert(body.end(),w.begin(),w.end());emit(body,spv::OpBitcast,{i.type,i.result,w[2]});}else body.insert(body.end(),w.begin(),w.end());continue;}
        if(i.op==spv::OpPhi||i.op==spv::OpSelect||i.op==spv::OpFunctionCall||i.op==spv::OpCopyMemory||i.op==spv::OpCopyMemorySized){
            for(size_t n=i.type?3:1;n<i.w.size();n++)if(signs.contains(i.w[n]))throw std::runtime_error("Unsupported integer output pointer use");}
        body.insert(body.end(),i.w.begin(),i.w.end());
    }
    std::vector<uint32_t> out(words.begin(),words.begin()+5);out[3]=next;
    for(const auto& i:code){if(i.op==spv::OpFunction)break;if(i.op!=spv::OpVariable)out.insert(out.end(),i.w.begin(),i.w.end());}
    out.insert(out.end(),declarations.begin(),declarations.end());out.insert(out.end(),globals.begin(),globals.end());out.insert(out.end(),body.begin(),body.end());return out;
}

std::vector<uint32_t> shader_without_unused_interlock(std::span<const uint32_t> words){
    auto code=parse(words);
    for(const auto& i:code){
        if(i.op==spv::OpBeginInvocationInterlockEXT||i.op==spv::OpEndInvocationInterlockEXT)return {words.begin(),words.end()};
        if(i.op==spv::OpExecutionMode&&i.w.size()>2&&i.w[2]>=5366&&i.w[2]<=5371)return {words.begin(),words.end()};
    }
    std::vector<uint32_t> out(words.begin(),words.begin()+5);
    for(const auto& i:code){
        if(i.op==spv::OpCapability&&i.w.size()==2&&(i.w[1]==spv::CapabilityFragmentShaderPixelInterlockEXT||i.w[1]==spv::CapabilityFragmentShaderSampleInterlockEXT||i.w[1]==spv::CapabilityFragmentShaderShadingRateInterlockEXT))continue;
        if(i.op==spv::OpExtension){const char* text=reinterpret_cast<const char*>(i.w.data()+1);const size_t bytes=(i.w.size()-1)*4;constexpr char name[]="SPV_EXT_fragment_shader_interlock";
            if(bytes>=sizeof(name)&&!memcmp(text,name,sizeof(name)))continue;
        }
        out.insert(out.end(),i.w.begin(),i.w.end());
    }
    return out;
}

std::vector<uint32_t> shader_null_descriptors(std::span<const uint32_t> words,std::span<const uint32_t> bindings){
    if(bindings.empty())return {words.begin(),words.end()};
    auto code=parse(words);std::set<uint32_t> wanted(bindings.begin(),bindings.end()),roots,removed,opaque_types;
    std::map<uint32_t,uint32_t> sets,binding_ids;
    for(const auto& i:code){
        if(i.op==spv::OpDecorate&&i.w.size()>=4){if(i.w[2]==spv::DecorationBinding)binding_ids[i.w[1]]=i.w[3];if(i.w[2]==spv::DecorationDescriptorSet)sets[i.w[1]]=i.w[3];}
        if(i.op==spv::OpTypePointer||i.op==spv::OpTypeImage||i.op==spv::OpTypeSampler||i.op==spv::OpTypeSampledImage)opaque_types.insert(i.result);
    }
    for(const auto& i:code)if(i.op==spv::OpVariable&&binding_ids.contains(i.result)&&wanted.contains(binding_ids[i.result])&&sets.contains(i.result)&&sets[i.result]==0)roots.insert(i.result);
    if(roots.empty())return {words.begin(),words.end()};
    removed=roots;
    // Pointer and opaque-value provenance. Numeric load results are replaced
    // below, so zeros flow through the original arithmetic/control flow.
    for(unsigned pass=0;pass<8;++pass){bool changed=false;
        for(const auto& i:code){
            if(!i.result||!opaque_types.contains(i.type)||removed.contains(i.result))continue;
            size_t operand=0;
            switch(i.op){
            case spv::OpAccessChain:case spv::OpInBoundsAccessChain:case spv::OpPtrAccessChain:case spv::OpInBoundsPtrAccessChain:
            case spv::OpLoad:case spv::OpCopyObject:case spv::OpImage:case spv::OpSampledImage:case spv::OpImageTexelPointer:operand=3;break;
            case spv::OpPhi:case spv::OpSelect:
                // Runtime selection between bound and unbound descriptors needs
                // a runtime guard; it cannot be silently specialized to zero.
                for(size_t n=i.op==spv::OpPhi?3:4;n<i.w.size();n+=i.op==spv::OpPhi?2:1)
                    if(removed.contains(i.w[n]))throw std::runtime_error("Selected null descriptor needs dynamic shader fallback");
                break;
            default:break;
            }
            if(operand&&i.w.size()>operand&&removed.contains(i.w[operand])){removed.insert(i.result);changed=true;}
        }
        if(!changed)break;if(pass==7)throw std::runtime_error("Shader descriptor provenance did not converge");
    }
    std::map<uint32_t,uint32_t> zeros;
    std::set<size_t> zero_instructions,drop_instructions;
    uint32_t next=words[3];
    for(size_t at=0;at<code.size();++at){const auto& i=code[at];
        if(i.result&&removed.contains(i.result)){drop_instructions.insert(at);continue;}
        size_t access=0;bool drop=false;
        switch(i.op){
        case spv::OpLoad:case spv::OpArrayLength:access=3;break;
        case spv::OpStore:case spv::OpAtomicStore:access=1;drop=true;break;
        case spv::OpAtomicLoad:case spv::OpAtomicExchange:case spv::OpAtomicCompareExchange:case spv::OpAtomicCompareExchangeWeak:
        case spv::OpAtomicIIncrement:case spv::OpAtomicIDecrement:case spv::OpAtomicIAdd:case spv::OpAtomicISub:
        case spv::OpAtomicSMin:case spv::OpAtomicUMin:case spv::OpAtomicSMax:case spv::OpAtomicUMax:
        case spv::OpAtomicAnd:case spv::OpAtomicOr:case spv::OpAtomicXor:access=3;break;
        case spv::OpImageWrite:access=1;drop=true;break;
        case spv::OpImageSampleImplicitLod:case spv::OpImageSampleExplicitLod:case spv::OpImageSampleDrefImplicitLod:case spv::OpImageSampleDrefExplicitLod:
        case spv::OpImageSampleProjImplicitLod:case spv::OpImageSampleProjExplicitLod:case spv::OpImageSampleProjDrefImplicitLod:case spv::OpImageSampleProjDrefExplicitLod:
        case spv::OpImageFetch:case spv::OpImageGather:case spv::OpImageDrefGather:case spv::OpImageRead:
        case spv::OpImageQueryFormat:case spv::OpImageQueryOrder:case spv::OpImageQuerySizeLod:case spv::OpImageQuerySize:case spv::OpImageQueryLod:case spv::OpImageQueryLevels:case spv::OpImageQuerySamples:access=3;break;
        default:break;
        }
        if(access&&i.w.size()>access&&removed.contains(i.w[access])){
            if(drop)drop_instructions.insert(at);
            else{if(!i.type||!i.result)throw std::runtime_error("Null access without typed result");zero_instructions.insert(at);if(!zeros.contains(i.type))zeros[i.type]=next++;}
        }
    }
    std::vector<uint32_t> out(words.begin(),words.begin()+5);out[3]=next;bool constants=false;
    for(size_t at=0;at<code.size();++at){const auto& i=code[at];
        if(!constants&&i.op==spv::OpFunction){for(auto [type,id]:zeros)emit(out,spv::OpConstantNull,{type,id});constants=true;}
        if(drop_instructions.contains(at))continue;
        if((i.op==spv::OpName||i.op==spv::OpDecorate)&&i.w.size()>1&&removed.contains(i.w[1]))continue;
        if(zero_instructions.contains(at)){emit(out,spv::OpCopyObject,{i.type,i.result,zeros.at(i.type)});continue;}
        if(i.op==spv::OpEntryPoint){
            auto entry=i.w;size_t first=3;
            for(;first<entry.size();++first){auto word=entry[first];if(!(word&255)||!(word&65280)||!(word&16711680)||!(word&4278190080u)){++first;break;}}
            entry.erase(std::remove_if(entry.begin()+first,entry.end(),[&](auto id){return removed.contains(id);}),entry.end());entry[0]=uint32_t(entry.size())<<16|uint32_t(i.op);out.insert(out.end(),entry.begin(),entry.end());continue;
        }
        out.insert(out.end(),i.w.begin(),i.w.end());
    }
    return out;
}

std::vector<uint32_t> shader_readonly_depth_bounds(std::span<const uint32_t> words){
    auto code=parse(words);uint32_t next=words[3],entry=0,entry_label=0;
    std::vector<uint32_t> declarations,decorations,body;
    bool input_capability=false;
    for(const auto& i:code){
        if(i.op==spv::OpEntryPoint){if(entry||i.w[1]!=spv::ExecutionModelFragment)throw std::runtime_error("Depth bounds requires one fragment entry");entry=i.w[2];}
        if(i.op==spv::OpCapability&&i.w[1]==spv::CapabilityInputAttachment)input_capability=true;
        if(i.op==spv::OpDecorate&&i.w.size()>=4&&i.w[2]==spv::DecorationBinding&&(i.w[3]==depth_bounds_uniform_binding||i.w[3]==depth_bounds_input_binding))throw std::runtime_error("Depth bounds descriptor binding collision");
    }
    if(!entry)throw std::runtime_error("Missing depth-bounds fragment entry");
    auto type=[&](spv::Op op,std::initializer_list<uint32_t> operands){
        for(const auto& i:code)if(i.op==op&&i.w.size()==operands.size()+2&&std::equal(operands.begin(),operands.end(),i.w.begin()+2))return i.result;
        auto id=next++;declarations.push_back(uint32_t(operands.size()+2)<<16|uint32_t(op));declarations.push_back(id);declarations.insert(declarations.end(),operands);return id;
    };
    const auto boolean=type(spv::OpTypeBool,{}),floating=type(spv::OpTypeFloat,{32}),integer=type(spv::OpTypeInt,{32,1});
    const auto ivec2=type(spv::OpTypeVector,{integer,2}),vec4=type(spv::OpTypeVector,{floating,4});
    const auto image=type(spv::OpTypeImage,{floating,spv::DimSubpassData,0,0,0,2,spv::ImageFormatUnknown});
    const auto image_ptr=type(spv::OpTypePointer,{spv::StorageClassUniformConstant,image});
    const auto input=next++;emit(declarations,spv::OpVariable,{image_ptr,input,spv::StorageClassUniformConstant});
    // A fresh block avoids attaching Block/member decorations to a guest type.
    const auto block=next++;emit(declarations,spv::OpTypeStruct,{block,floating,floating});
    const auto block_ptr=type(spv::OpTypePointer,{spv::StorageClassUniform,block});
    const auto float_ptr=type(spv::OpTypePointer,{spv::StorageClassUniform,floating});
    const auto parameters=next++;emit(declarations,spv::OpVariable,{block_ptr,parameters,spv::StorageClassUniform});
    const auto zero=next++,one=next++,coordinate=next++;
    emit(declarations,spv::OpConstant,{integer,zero,0});emit(declarations,spv::OpConstant,{integer,one,1});emit(declarations,spv::OpConstantNull,{ivec2,coordinate});
    emit(decorations,spv::OpDecorate,{input,spv::DecorationDescriptorSet,0});emit(decorations,spv::OpDecorate,{input,spv::DecorationBinding,depth_bounds_input_binding});emit(decorations,spv::OpDecorate,{input,spv::DecorationInputAttachmentIndex,0});
    emit(decorations,spv::OpDecorate,{block,spv::DecorationBlock});emit(decorations,spv::OpMemberDecorate,{block,0,spv::DecorationOffset,0});emit(decorations,spv::OpMemberDecorate,{block,1,spv::DecorationOffset,4});
    emit(decorations,spv::OpDecorate,{parameters,spv::DecorationDescriptorSet,0});emit(decorations,spv::OpDecorate,{parameters,spv::DecorationBinding,depth_bounds_uniform_binding});
    auto load_image=next++,pixel=next++,depth=next++,min_ptr=next++,max_ptr=next++,minimum=next++,maximum=next++,above=next++,below=next++,inside=next++,discard=next++,resume=next++;
    emit(body,spv::OpLoad,{image,load_image,input});emit(body,spv::OpImageRead,{vec4,pixel,load_image,coordinate});emit(body,spv::OpCompositeExtract,{floating,depth,pixel,0});
    emit(body,spv::OpAccessChain,{float_ptr,min_ptr,parameters,zero});emit(body,spv::OpAccessChain,{float_ptr,max_ptr,parameters,one});
    emit(body,spv::OpLoad,{floating,minimum,min_ptr});emit(body,spv::OpLoad,{floating,maximum,max_ptr});
    emit(body,spv::OpFOrdGreaterThanEqual,{boolean,above,depth,minimum});emit(body,spv::OpFOrdLessThanEqual,{boolean,below,depth,maximum});emit(body,spv::OpLogicalAnd,{boolean,inside,above,below});
    emit(body,spv::OpSelectionMerge,{resume,spv::SelectionControlMaskNone});emit(body,spv::OpBranchConditional,{inside,resume,discard});emit(body,spv::OpLabel,{discard});emit(body,spv::OpKill,{});emit(body,spv::OpLabel,{resume});
    std::vector<uint32_t> out(words.begin(),words.begin()+5);out[3]=next;
    bool cap_done=false,decorated=false,declared=false,in_entry=false,in_first_block=false,injected=false;
    for(const auto& i:code){
        if(!cap_done&&i.op!=spv::OpCapability){if(!input_capability)emit(out,spv::OpCapability,{spv::CapabilityInputAttachment});cap_done=true;}
        if(!decorated&&i.op>=spv::OpTypeVoid&&i.op<=spv::OpTypeForwardPointer){out.insert(out.end(),decorations.begin(),decorations.end());decorated=true;}
        if(!declared&&i.op==spv::OpFunction){out.insert(out.end(),declarations.begin(),declarations.end());declared=true;}
        if(i.op==spv::OpExecutionMode&&i.w[1]==entry&&i.w[2]==spv::ExecutionModeEarlyFragmentTests)continue;
        if(i.op==spv::OpFunction)in_entry=i.result==entry;
        if(in_entry&&i.op==spv::OpLabel&&!entry_label){entry_label=i.result;in_first_block=true;out.insert(out.end(),i.w.begin(),i.w.end());continue;}
        if(in_first_block&&!injected&&i.op!=spv::OpVariable&&i.op!=spv::OpLine&&i.op!=spv::OpNoLine){out.insert(out.end(),body.begin(),body.end());injected=true;}
        auto instruction=i.w;
        if(i.op==spv::OpPhi&&in_entry)for(size_t n=4;n<instruction.size();n+=2)if(instruction[n]==entry_label)instruction[n]=resume;
        if(i.op==spv::OpEntryPoint&&words[1]>=0x10400){instruction.push_back(input);instruction.push_back(parameters);instruction[0]=uint32_t(instruction.size())<<16|uint32_t(i.op);}
        out.insert(out.end(),instruction.begin(),instruction.end());
    }
    if(!decorated||!declared||!injected)throw std::runtime_error("Depth bounds could not locate a structured shader entry");
    return out;
}

std::vector<uint32_t> shader_count_samples(std::span<const uint32_t> words){
    auto code=parse(words);uint32_t next=words[3],entry=0,helper=0;
    std::vector<uint32_t> declarations,decorations;
    for(const auto& i:code){
        if(i.op==spv::OpEntryPoint){if(entry||i.w[1]!=spv::ExecutionModelFragment)throw std::runtime_error("Sample counter requires one fragment entry");entry=i.w[2];}
        if(i.op==spv::OpExecutionMode&&(i.w[2]==spv::ExecutionModeEarlyFragmentTests||i.w[2]==spv::ExecutionModeEarlyAndLateFragmentTestsAMD))throw std::runtime_error("Early sample counting needs native occlusion");
        if(i.op==spv::OpMemberDecorate&&i.w.size()>=5&&i.w[3]==spv::DecorationBuiltIn&&i.w[4]==spv::BuiltInSampleMask)throw std::runtime_error("Sample-mask shader needs native occlusion");
        if(i.op==spv::OpDecorate&&i.w.size()>=4){
            if(i.w[2]==spv::DecorationBinding&&i.w[3]==software_samples_binding)throw std::runtime_error("Sample counter descriptor binding collision");
            if(i.w[2]==spv::DecorationBuiltIn&&i.w[3]==spv::BuiltInHelperInvocation)helper=i.w[1];
            if(i.w[2]==spv::DecorationBuiltIn&&i.w[3]==spv::BuiltInSampleMask)throw std::runtime_error("Sample-mask shader needs native occlusion");
        }
    }
    if(!entry)throw std::runtime_error("Missing sample-counter fragment entry");
    auto type=[&](spv::Op op,std::initializer_list<uint32_t> operands){
        for(const auto& i:code)if(i.op==op&&i.w.size()==operands.size()+2&&std::equal(operands.begin(),operands.end(),i.w.begin()+2))return i.result;
        auto id=next++;declarations.push_back(uint32_t(operands.size()+2)<<16|uint32_t(op));declarations.push_back(id);declarations.insert(declarations.end(),operands);return id;
    };
    const auto boolean=type(spv::OpTypeBool,{}),integer=type(spv::OpTypeInt,{32,0});
    const auto block=next++;emit(declarations,spv::OpTypeStruct,{block,integer,integer});
    const auto block_ptr=type(spv::OpTypePointer,{spv::StorageClassStorageBuffer,block});
    const auto uint_ptr=type(spv::OpTypePointer,{spv::StorageClassStorageBuffer,integer});
    const auto counter=next++;emit(declarations,spv::OpVariable,{block_ptr,counter,spv::StorageClassStorageBuffer});
    const bool new_helper=!helper;
    if(new_helper){auto pointer=type(spv::OpTypePointer,{spv::StorageClassInput,boolean});helper=next++;emit(declarations,spv::OpVariable,{pointer,helper,spv::StorageClassInput});emit(decorations,spv::OpDecorate,{helper,spv::DecorationBuiltIn,spv::BuiltInHelperInvocation});}
    const auto zero=next++,one=next++,maximum=next++;
    emit(declarations,spv::OpConstant,{integer,zero,0});emit(declarations,spv::OpConstant,{integer,one,1});emit(declarations,spv::OpConstant,{integer,maximum,UINT32_MAX});
    emit(decorations,spv::OpDecorate,{block,spv::DecorationBlock});emit(decorations,spv::OpMemberDecorate,{block,0,spv::DecorationOffset,0});emit(decorations,spv::OpMemberDecorate,{block,1,spv::DecorationOffset,4});
    emit(decorations,spv::OpDecorate,{counter,spv::DecorationDescriptorSet,0});emit(decorations,spv::OpDecorate,{counter,spv::DecorationBinding,software_samples_binding});
    std::vector<uint32_t> out(words.begin(),words.begin()+5);out[1]=std::max(words[1],0x10300u);
    bool decorated=false,declared=false,in_entry=false;unsigned returns=0;
    for(const auto& i:code){
        if(!decorated&&i.op>=spv::OpTypeVoid&&i.op<=spv::OpTypeForwardPointer){out.insert(out.end(),decorations.begin(),decorations.end());decorated=true;}
        if(!declared&&i.op==spv::OpFunction){out.insert(out.end(),declarations.begin(),declarations.end());declared=true;}
        if(i.op==spv::OpFunction)in_entry=i.result==entry;
        if(in_entry&&i.op==spv::OpReturn){
            auto h=next++,amount=next++,lo=next++,hi=next++,old=next++,overflow=next++,carry=next++,ignored=next++;
            emit(out,spv::OpLoad,{boolean,h,helper});emit(out,spv::OpSelect,{integer,amount,h,zero,one});
            emit(out,spv::OpAccessChain,{uint_ptr,lo,counter,zero});emit(out,spv::OpAccessChain,{uint_ptr,hi,counter,one});
            emit(out,spv::OpAtomicIAdd,{integer,old,lo,one,zero,amount});
            emit(out,spv::OpIEqual,{boolean,overflow,old,maximum});emit(out,spv::OpSelect,{integer,carry,overflow,amount,zero});
            emit(out,spv::OpAtomicIAdd,{integer,ignored,hi,one,zero,carry});++returns;
        }
        auto instruction=i.w;
        if(i.op==spv::OpEntryPoint){if(new_helper)instruction.push_back(helper);if(out[1]>=0x10400)instruction.push_back(counter);instruction[0]=uint32_t(instruction.size())<<16|uint32_t(i.op);}
        out.insert(out.end(),instruction.begin(),instruction.end());
    }
    if(!declared||!decorated||!returns)throw std::runtime_error("Sample counter has no normal fragment return");
    out[3]=next;return out;
}
