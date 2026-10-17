// Stable recipes contain shader content keys and pointer-free pipeline state.
// The cache is optional: bad/missing records skip prewarming; foreground
// creation remains authoritative. Device/driver changes get a new
// directory. Recipe/SPIR-V content keys isolate changed shaders within it.
std::string pipeline_directory;
std::string pipeline_seed_directory;
std::vector<std::string> pipeline_legacy_directories;
size_t pipeline_spv_bytes=0,pipeline_recipe_bytes=0;
constexpr size_t pipeline_spv_limit=256*1024*1024,pipeline_recipe_limit=32*1024*1024;
std::mutex module_mutex,pipeline_work_mutex;
struct ModuleRecord {VkShaderModule module{};std::vector<uint32_t> words;};
std::map<std::string,ModuleRecord> module_records;
std::map<VkShaderModule,std::string> module_keys;
std::set<std::string> recorded_recipes;
std::atomic<uint64_t> warm_created{0},foreground_created{0},warm_claimed{0},warm_rejected{0};
std::atomic<bool> warm_running{false};
std::mutex warm_done_mutex;std::condition_variable warm_done;

std::string content_key(const void* data,size_t size){
    uint64_t hash=14695981039346656037ULL;auto* p=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;i++)hash=(hash^p[i])*1099511628211ULL;
    char text[17];snprintf(text,sizeof(text),"%016llx",(unsigned long long)hash);return text;
}
#include "driver_cache.inc"
bool valid_module_key(const std::string& key){return key.size()==16&&key.find_first_not_of("0123456789abcdef")==std::string::npos;}
void persist_module(const std::string& key,const ModuleRecord& record){
    if(pipeline_directory.empty())return;
    const auto path=pipeline_directory+"/"+key+".spv";
    // Existence alone is not enough: a damaged optional copy must be repaired
    // when the foreground loads the authoritative offline shader again.
    std::ifstream existing(path,std::ios::binary|std::ios::ate);
    if(existing&&existing.tellg()==std::streamoff(record.words.size()*4)){
        std::vector<uint32_t> previous(record.words.size());existing.seekg(0);
        existing.read(reinterpret_cast<char*>(previous.data()),previous.size()*4);
        if(existing&&previous==record.words)return;
    }
    existing.close();std::error_code error;
    const auto previous=std::filesystem::file_size(path,error);const auto replaced=error?size_t(0):size_t(previous);
    const auto retained=pipeline_spv_bytes-std::min(pipeline_spv_bytes,replaced);
    if(record.words.size()*4>pipeline_spv_limit-retained)return;
    std::ofstream out(path+".tmp",std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(record.words.data()),record.words.size()*4);out.close();
    if(out){std::filesystem::rename(path+".tmp",path,error);if(!error)pipeline_spv_bytes=retained+record.words.size()*4;}
}
VkShaderModule shared_shader_module(const uint32_t* words,size_t count){
    auto key=content_key(words,count*4);std::lock_guard lock(module_mutex);
    if(auto it=module_records.find(key);it!=module_records.end()){
        if(it->second.words.size()!=count||memcmp(it->second.words.data(),words,count*4))ot2_host_die("Shader content key collision");
        return it->second.module;
    }
    ModuleRecord record;record.words.assign(words,words+count);
    const auto portable=shader_without_unused_interlock({words,count});
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=portable.size()*4;ci.pCode=portable.data();
    check(vkCreateShaderModule(device,&ci,nullptr,&record.module),"shared shader module");
    module_keys.emplace(record.module,key);auto [it,inserted]=module_records.emplace(key,std::move(record));
    persist_module(key,it->second);return it->second.module;
}
std::string module_key(VkShaderModule module){std::lock_guard lock(module_mutex);return module_keys.at(module);}
std::mutex depth_compat_mutex;
std::map<VkShaderModule,VkShaderModule> depth_compat_modules;
VkShaderModule depth_compatible_module(VkShaderModule module){
    std::lock_guard lock(depth_compat_mutex);
    if(auto found=depth_compat_modules.find(module);found!=depth_compat_modules.end())return found->second;
    if(depth_compat_modules.size()>=4096)throw std::runtime_error("depth compatibility variant limit exceeded");
    std::vector<uint32_t> words;{std::lock_guard module_lock(module_mutex);words=module_records.at(module_keys.at(module)).words;}
    auto patched=shader_readonly_depth_bounds(words);auto result=shared_shader_module(patched.data(),patched.size());
    depth_compat_modules.emplace(module,result);return result;
}
VkShaderModule warm_module(const std::string& key){
    if(!valid_module_key(key))throw std::runtime_error("invalid module key");
    {std::lock_guard lock(module_mutex);if(auto it=module_records.find(key);it!=module_records.end())return it->second.module;}
    std::ifstream in(pipeline_directory+"/"+key+".spv",std::ios::binary|std::ios::ate);
    for(const auto& directory:pipeline_legacy_directories)if(!in){in.clear();in.open(directory+"/"+key+".spv",std::ios::binary|std::ios::ate);}
    if(!in&&!pipeline_seed_directory.empty()){in.clear();in.open(pipeline_seed_directory+"/"+key+".spv",std::ios::binary|std::ios::ate);}
    if(!in)throw std::runtime_error("missing module");
    auto bytes=in.tellg();if(bytes<20||bytes>8*1024*1024||size_t(bytes)%4)throw std::runtime_error("invalid module size");
    std::vector<uint32_t> words(size_t(bytes)/4);in.seekg(0);in.read(reinterpret_cast<char*>(words.data()),bytes);
    if(!in||words[0]!=0x07230203||content_key(words.data(),size_t(bytes))!=key)throw std::runtime_error("module checksum");
    return shared_shader_module(words.data(),words.size());
}
template<class T>std::string recipe_blob(const T* values,size_t count=1){
    static constexpr char hex[]="0123456789abcdef";auto* bytes=reinterpret_cast<const uint8_t*>(values);std::string out(count*sizeof(T)*2,'0');
    for(size_t i=0;i<count*sizeof(T);i++){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15];}return out;
}
template<class T>std::vector<T> recipe_values(const Json& j,size_t maximum){
    auto text=j.get<std::string>();if(text.size()%(2*sizeof(T))||text.size()>maximum*2*sizeof(T))throw std::runtime_error("recipe field size");
    auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("recipe hex");};
    std::vector<T> out(text.size()/(2*sizeof(T)));auto* bytes=reinterpret_cast<uint8_t*>(out.data());
    for(size_t i=0;i<text.size()/2;i++)bytes[i]=uint8_t(digit(text[2*i])*16+digit(text[2*i+1]));return out;
}
template<class T>T recipe_value(const Json& j){auto values=recipe_values<T>(j,1);if(values.size()!=1)throw std::runtime_error("missing recipe field");return values[0];}

struct PipelineRecipe {
    bool compute=false;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkPushConstantRange> pushes;
    std::vector<VkFormat> colors;
    std::vector<VkVertexInputBindingDescription> vertices;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    std::vector<VkDynamicState> dynamics;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkSampleMask mask=~0u;
    bool valid_dynamics()const{return dynamics==std::vector<VkDynamicState>{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR}||dynamics==std::vector<VkDynamicState>{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BOUNDS};}
    void canonical_bounds(){if(dynamics.size()==3){depth.minDepthBounds=0;depth.maxDepthBounds=1;}}
    static std::vector<VkPipelineShaderStageCreateInfo> stage_copy(const VkPipelineShaderStageCreateInfo* input,size_t count){
        std::vector<VkPipelineShaderStageCreateInfo> out(input,input+count);
        for(auto& s:out)if(s.pNext||s.flags||s.pSpecializationInfo||!s.pName||strcmp(s.pName,"main"))throw std::runtime_error("unsupported pipeline stage recipe");
        for(auto& s:out)s.pName="main";return out;
    }
    PipelineRecipe(const VkComputePipelineCreateInfo& ci,const std::vector<VkDescriptorSetLayoutBinding>& bs):compute(true),stages(stage_copy(&ci.stage,1)),bindings(bs){if(ci.flags||ci.pNext)throw std::runtime_error("unsupported compute recipe");}
    PipelineRecipe(const VkGraphicsPipelineCreateInfo& ci,const std::vector<VkDescriptorSetLayoutBinding>& bs,const std::vector<VkPushConstantRange>& ps):stages(stage_copy(ci.pStages,ci.stageCount)),bindings(bs),pushes(ps){
        if(ci.flags||ci.renderPass||ci.pTessellationState||!ci.pNext)throw std::runtime_error("unsupported graphics recipe");
        rendering=*static_cast<const VkPipelineRenderingCreateInfo*>(ci.pNext);
        if(rendering.sType!=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO||rendering.pNext)throw std::runtime_error("unsupported rendering recipe");
        if(rendering.colorAttachmentCount)colors.assign(rendering.pColorAttachmentFormats,rendering.pColorAttachmentFormats+rendering.colorAttachmentCount);
        const auto& vi=*ci.pVertexInputState;
        if(vi.vertexBindingDescriptionCount)vertices.assign(vi.pVertexBindingDescriptions,vi.pVertexBindingDescriptions+vi.vertexBindingDescriptionCount);
        if(vi.vertexAttributeDescriptionCount)attributes.assign(vi.pVertexAttributeDescriptions,vi.pVertexAttributeDescriptions+vi.vertexAttributeDescriptionCount);
        assembly=*ci.pInputAssemblyState;raster=*ci.pRasterizationState;multisample=*ci.pMultisampleState;depth=*ci.pDepthStencilState;blend=*ci.pColorBlendState;viewport=*ci.pViewportState;
        if(blend.attachmentCount)blends.assign(blend.pAttachments,blend.pAttachments+blend.attachmentCount);
        if(ci.pDynamicState)dynamics.assign(ci.pDynamicState->pDynamicStates,ci.pDynamicState->pDynamicStates+ci.pDynamicState->dynamicStateCount);
        if(!valid_dynamics())throw std::runtime_error("unsupported dynamic recipe");
        canonical_bounds();
        if(assembly.pNext||raster.pNext||multisample.pNext||depth.pNext||blend.pNext||viewport.pNext)throw std::runtime_error("unsupported state extension recipe");
        mask=multisample.pSampleMask?*multisample.pSampleMask:~0u;
        rendering.pColorAttachmentFormats=nullptr;multisample.pSampleMask=nullptr;blend.pAttachments=nullptr;viewport.pViewports=nullptr;viewport.pScissors=nullptr;
    }
    Json json()const{
        Json j={{"schema",1},{"compute",compute},{"stages",Json::array()},{"bindings",Json::array()},{"push",recipe_blob(pushes.data(),pushes.size())}};
        for(auto& s:stages)j["stages"].push_back({uint32_t(s.stage),module_key(s.module)});
        for(auto& b:bindings){if(b.pImmutableSamplers)throw std::runtime_error("immutable sampler recipe");j["bindings"].push_back({b.binding,uint32_t(b.descriptorType),b.descriptorCount,b.stageFlags});}
        if(!compute){j["rendering"]=recipe_blob(&rendering);j["colors"]=recipe_blob(colors.data(),colors.size());j["vertices"]=recipe_blob(vertices.data(),vertices.size());j["attributes"]=recipe_blob(attributes.data(),attributes.size());j["assembly"]=recipe_blob(&assembly);j["raster"]=recipe_blob(&raster);j["multisample"]=recipe_blob(&multisample);j["depth"]=recipe_blob(&depth);j["blend"]=recipe_blob(&blend);j["blends"]=recipe_blob(blends.data(),blends.size());j["viewport"]=recipe_blob(&viewport);j["dynamics"]=recipe_blob(dynamics.data(),dynamics.size());j["mask"]=mask;}
        return j;
    }
    PipelineRecipe(const Json& j,bool migrate_bounds){
        if(j.at("schema")!=1)throw std::runtime_error("recipe schema");compute=j.at("compute").get<bool>();
        if(j.at("stages").size()>6||j.at("stages").empty()||j.at("bindings").size()>1024)throw std::runtime_error("recipe limits");
        for(auto& s:j.at("stages")){VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VkShaderStageFlagBits(s.at(0).get<uint32_t>());stage.module=warm_module(s.at(1).get<std::string>());stage.pName="main";stages.push_back(stage);}
        for(auto& b:j.at("bindings"))bindings.push_back({b.at(0).get<uint32_t>(),VkDescriptorType(b.at(1).get<uint32_t>()),b.at(2).get<uint32_t>(),b.at(3).get<uint32_t>(),nullptr});
        pushes=recipe_values<VkPushConstantRange>(j.at("push"),8);
        if(compute){if(stages.size()!=1||stages[0].stage!=VK_SHADER_STAGE_COMPUTE_BIT)throw std::runtime_error("compute stage");return;}
        rendering=recipe_value<VkPipelineRenderingCreateInfo>(j.at("rendering"));colors=recipe_values<VkFormat>(j.at("colors"),8);
        vertices=recipe_values<VkVertexInputBindingDescription>(j.at("vertices"),16);attributes=recipe_values<VkVertexInputAttributeDescription>(j.at("attributes"),16);
        assembly=recipe_value<VkPipelineInputAssemblyStateCreateInfo>(j.at("assembly"));raster=recipe_value<VkPipelineRasterizationStateCreateInfo>(j.at("raster"));multisample=recipe_value<VkPipelineMultisampleStateCreateInfo>(j.at("multisample"));depth=recipe_value<VkPipelineDepthStencilStateCreateInfo>(j.at("depth"));blend=recipe_value<VkPipelineColorBlendStateCreateInfo>(j.at("blend"));viewport=recipe_value<VkPipelineViewportStateCreateInfo>(j.at("viewport"));
        blends=recipe_values<VkPipelineColorBlendAttachmentState>(j.at("blends"),8);dynamics=recipe_values<VkDynamicState>(j.at("dynamics"),3);mask=j.at("mask").get<uint32_t>();
        if(rendering.pNext||rendering.pColorAttachmentFormats||rendering.colorAttachmentCount!=colors.size()||assembly.pNext||raster.pNext||multisample.pNext||multisample.pSampleMask||depth.pNext||blend.pNext||blend.pAttachments||blend.attachmentCount!=blends.size()||viewport.pNext||viewport.pViewports||viewport.pScissors||viewport.viewportCount!=1||viewport.scissorCount!=1)throw std::runtime_error("recipe pointer/count mismatch");
        if(rendering.sType!=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO||assembly.sType!=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO||raster.sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO||multisample.sType!=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO||depth.sType!=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO||blend.sType!=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO||viewport.sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||!valid_dynamics())throw std::runtime_error("recipe state schema");
        // Migrate old game-draw recipes without deleting the on-disk cache.
        // Host clear pipelines have push constants and stay static. Duplicate
        // migrated ranges converge through the ordinary single-flight key.
        if(migrate_bounds&&pushes.empty()&&dynamics.size()==2)dynamics.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
        canonical_bounds();
        if(!depth_bounds_supported&&depth.depthBoundsTestEnable){
            bool uniform=false,input=false;
            for(const auto& b:bindings){uniform|=b.binding==depth_bounds_uniform_binding;input|=b.binding==depth_bounds_input_binding;}
            if(uniform!=input)throw std::runtime_error("incomplete software depth recipe");
            if(!input){
                if(depth.depthWriteEnable||depth.stencilTestEnable||multisample.rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT||rendering.depthAttachmentFormat==VK_FORMAT_UNDEFINED)throw std::runtime_error("unsupported writable depth recipe");
                bool fragment=false;
                for(auto& stage:stages)if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT){
                    stage.module=depth_compatible_module(stage.module);fragment=true;
                }
                if(!fragment)throw std::runtime_error("software depth recipe needs a fragment stage");
                bindings.push_back({depth_bounds_uniform_binding,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
                bindings.push_back({depth_bounds_input_binding,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
            }
        }
    }
    template<class F>auto with_info(VkPipelineLayout layout,F&& f){
        VkComputePipelineCreateInfo compute_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute_info.stage=stages[0];compute_info.layout=layout;
        VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=vertices.size();vi.pVertexBindingDescriptions=vertices.data();vi.vertexAttributeDescriptionCount=attributes.size();vi.pVertexAttributeDescriptions=attributes.data();
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=dynamics.size();dynamic.pDynamicStates=dynamics.data();
        auto rendering_info=rendering;rendering_info.pColorAttachmentFormats=colors.data();auto ms=multisample;ms.pSampleMask=&mask;auto cb=blend;cb.pAttachments=blends.data();
        graphics.pNext=&rendering_info;graphics.stageCount=stages.size();graphics.pStages=stages.data();graphics.pVertexInputState=&vi;graphics.pInputAssemblyState=&assembly;graphics.pViewportState=&viewport;graphics.pRasterizationState=&raster;graphics.pMultisampleState=&ms;graphics.pDepthStencilState=&depth;graphics.pColorBlendState=&cb;graphics.pDynamicState=&dynamic;graphics.layout=layout;
        return f(compute_info,graphics);
    }
    std::string key(){auto key=with_info(VK_NULL_HANDLE,[&](auto& c,auto& g){return compute?pipeline_key(c,bindings):pipeline_key(g,bindings);});key+=recipe_blob(pushes.data(),pushes.size());return key;}
};

// A pipeline retains its creation layout on devices without maintenance4.
// Descriptor arenas may retire independently, so retain an equivalent immutable
// layout here for both foreground and prewarmed pipelines.
struct PipelineLayoutLifetime {
    VkDescriptorSetLayout descriptor{};VkPipelineLayout layout{};
    ~PipelineLayoutLifetime(){if(layout)vkDestroyPipelineLayout(device,layout,nullptr);if(descriptor)vkDestroyDescriptorSetLayout(device,descriptor,nullptr);}
};
std::mutex pipeline_layout_mutex;
std::map<std::string,std::weak_ptr<PipelineLayoutLifetime>> pipeline_layouts;
std::shared_ptr<PipelineLayoutLifetime> pipeline_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings,const std::vector<VkPushConstantRange>& pushes={}){
    const auto key=recipe_blob(bindings.data(),bindings.size())+":"+recipe_blob(pushes.data(),pushes.size());
    std::lock_guard lock(pipeline_layout_mutex);
    if(auto found=pipeline_layouts.find(key);found!=pipeline_layouts.end())if(auto live=found->second.lock())return live;
    auto result=std::make_shared<PipelineLayoutLifetime>();
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=bindings.size();di.pBindings=bindings.data();
    check(vkCreateDescriptorSetLayout(device,&di,nullptr,&result->descriptor),"pipeline descriptor layout");
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=bindings.empty()&&!pushes.empty()?0:1;li.pSetLayouts=&result->descriptor;li.pushConstantRangeCount=pushes.size();li.pPushConstantRanges=pushes.data();
    check(vkCreatePipelineLayout(device,&li,nullptr,&result->layout),"pipeline retained layout");
    if(pipeline_layouts.size()>=512){
        for(auto it=pipeline_layouts.begin();it!=pipeline_layouts.end();){
            if(it->second.expired())it=pipeline_layouts.erase(it);else ++it;
        }
    }
    if(pipeline_layouts.size()<512)pipeline_layouts[key]=result;
    return result;
}
struct PipelineWork {
    std::mutex mutex;std::condition_variable changed;bool done=false,warm=false;VkPipeline pipeline{};
    std::shared_ptr<PipelineLayoutLifetime> layout;
};
std::map<std::string,std::shared_ptr<PipelineWork>> pipeline_work;
template<class Build>VkPipeline request_pipeline(const std::string& key,bool warm,Build&& build){
    std::shared_ptr<PipelineWork> work;bool owner=false;
    {std::lock_guard lock(pipeline_work_mutex);auto [it,inserted]=pipeline_work.try_emplace(key,std::make_shared<PipelineWork>());work=it->second;owner=inserted;}
    if(!owner){
        std::unique_lock lock(work->mutex);work->changed.wait(lock,[&]{return work->done;});
        if(work->pipeline){if(!warm&&work->warm)++warm_claimed;return work->pipeline;}
        // A failed optional warm-up must not permanently poison the real draw.
        if(warm)return VK_NULL_HANDLE;work->done=false;owner=true;
    }
    VkPipeline pipeline{};
    try {if constexpr(std::is_invocable_v<Build,PipelineWork&>)pipeline=build(*work);else pipeline=build();}
    catch(...){
        {std::lock_guard lock(work->mutex);work->done=true;work->layout.reset();}
        work->changed.notify_all();throw;
    }
    {std::lock_guard lock(work->mutex);work->pipeline=pipeline;work->warm=warm;work->done=true;}
    if(pipeline){if(warm)++warm_created;else ++foreground_created;}work->changed.notify_all();return pipeline;
}
void record_pipeline(const PipelineRecipe& recipe){
    if(pipeline_directory.empty())return;
    try{auto j=recipe.json();auto text=j.dump();if(recorded_recipes.contains(text)||recorded_recipes.size()>=8192)return;
        Json envelope={{"recipe",std::move(j)},{"checksum",content_key(text.data(),text.size())}};
        auto line=envelope.dump();if(line.size()+1>pipeline_recipe_limit-pipeline_recipe_bytes)return;
        std::ofstream out(pipeline_directory+"/manifest.jsonl",std::ios::app);out<<line<<'\n';
        if(out){recorded_recipes.insert(text);pipeline_recipe_bytes+=line.size()+1;}
    }catch(const std::exception& e){ot2_host_log("PIPELINE_RECORD_SKIP %s",e.what());}
}
VkPipeline cached_pipeline(const VkComputePipelineCreateInfo& ci,const std::vector<VkDescriptorSetLayoutBinding>& bindings){
    PerfScope timing(perf.pipeline_ns);auto key=pipeline_key(ci,bindings);if(auto it=pipeline_objects.find(key);it!=pipeline_objects.end())return it->second;
    auto pipeline=request_pipeline(key,false,[&](PipelineWork& work){work.layout=pipeline_layout(bindings);auto retained=ci;retained.layout=work.layout->layout;std::lock_guard lock(driver_cache_mutex);VkPipeline p{};check(vkCreateComputePipelines(device,pipeline_cache,1,&retained,nullptr,&p),"compute pipeline");return p;});
    pipeline_objects.emplace(key,pipeline);try{record_pipeline(PipelineRecipe(ci,bindings));}catch(const std::exception& e){ot2_host_log("PIPELINE_RECORD_SKIP %s",e.what());}return pipeline;
}
VkPipeline cached_pipeline(const VkGraphicsPipelineCreateInfo& ci,const std::vector<VkDescriptorSetLayoutBinding>& bindings,const std::vector<VkPushConstantRange>& pushes={}){
    PerfScope timing(perf.pipeline_ns);auto key=pipeline_key(ci,bindings)+recipe_blob(pushes.data(),pushes.size());if(auto it=pipeline_objects.find(key);it!=pipeline_objects.end())return it->second;
    auto pipeline=request_pipeline(key,false,[&](PipelineWork& work){work.layout=pipeline_layout(bindings,pushes);auto retained=ci;retained.layout=work.layout->layout;std::lock_guard lock(driver_cache_mutex);VkPipeline p{};check(create_graphics_pipeline(pipeline_cache,retained,&p),"graphics pipeline");return p;});
    pipeline_objects.emplace(key,pipeline);try{record_pipeline(PipelineRecipe(ci,bindings,pushes));}catch(const std::exception& e){ot2_host_log("PIPELINE_RECORD_SKIP %s",e.what());}return pipeline;
}
void start_pipeline_warmup(){
    if(!device||shader_directory.empty()||!pipeline_directory.empty())return;
    dynamic_depth_bounds=!std::filesystem::exists(shader_directory+"/no-dynamic-depth-bounds");
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    // Recipes identify exact SPIR-V contents and complete pipeline state. A
    // package manifest/version change cannot invalidate unchanged shader code.
    // The enclosing shader_directory is already isolated per game profile.
    auto identity=driver_policy_identity();
    auto path=shader_directory+"/pipelines-v1/"+content_key(identity.data(),identity.size());std::error_code error;std::filesystem::create_directories(path,error);
    if(error){ot2_host_log("PIPELINE_CACHE_DISABLED %s",error.message().c_str());return;}
    for(const auto& entry:std::filesystem::directory_iterator(path,error))if(entry.path().extension()==".spv"){
        auto bytes=entry.file_size(error);if(!error)pipeline_spv_bytes=std::min(pipeline_spv_limit,pipeline_spv_bytes+size_t(bytes));
    }
    {std::lock_guard lock(module_mutex);pipeline_directory=path;for(auto& [key,module]:module_records)persist_module(key,module);}
    persist_driver_cache=!std::filesystem::exists(shader_directory+"/no-driver-cache");load_driver_cache(path);
    // Import at most three previous package-local caches from this same game's
    // directory, only after driver UUID/header/checksum validation. All modules
    // and recipes are independently checked again; these remain optional hints.
    std::vector<std::pair<std::filesystem::file_time_type,std::string>> previous;
    for(std::filesystem::directory_iterator it(shader_directory+"/pipelines-v1",error),end;!error&&it!=end;it.increment(error)){
        std::error_code entry_error;
        if(it->path()==path||!it->is_directory(entry_error)||entry_error)continue;
        const auto modified=it->last_write_time(entry_error);if(entry_error)continue;
        previous.emplace_back(modified,it->path().string());
    }
    std::sort(previous.rbegin(),previous.rend());
    for(const auto& [modified,directory]:previous){if(std::filesystem::exists(path+"/manifest.jsonl")||pipeline_legacy_directories.size()==3)break;if(read_driver_cache(directory).empty())continue;
        pipeline_legacy_directories.push_back(directory);load_driver_cache(directory);
    }
    ot2_host_log("PIPELINE_IDENTITY content_addressed=1 imported_directories=%zu",pipeline_legacy_directories.size());
    std::vector<Json> records;std::string line;size_t bytes=0;
    auto import_recipes=[&](const std::string& directory){std::ifstream input(directory+"/manifest.jsonl");
    while(records.size()<8192&&std::getline(input,line)){
        if((bytes+=line.size())>pipeline_recipe_limit)break;
        try{if(line.size()>262144)throw std::runtime_error("recipe too large");auto envelope=Json::parse(line);auto& recipe=envelope.at("recipe");auto text=recipe.dump();if(envelope.at("checksum")!=content_key(text.data(),text.size()))throw std::runtime_error("recipe checksum");if(recorded_recipes.insert(text).second)records.push_back(recipe);}catch(...){++warm_rejected;}
    }};
    import_recipes(path);for(const auto& directory:pipeline_legacy_directories)import_recipes(directory);
    // Portable seeds contain observed recipes and SPIR-V, never another GPU's
    // driver binary. Match the immutable shader-set identity and recipe ABI.
    // Ordinary driver-local records retain priority, including corruption
    // recovery tests. A missing/bad seed cannot poison foreground creation.
    if(!std::filesystem::exists(shader_directory+"/no-pipeline-seed"))try{
        const auto seed=shader_directory+"/pipeline-seed-v1";
        std::ifstream metadata(seed+"/seed.json");Json info;
        if(metadata){metadata>>info;
            const auto shader_set=std::filesystem::path(shader_binary_directory).filename().string();
            if(info.at("version")!=1||info.at("shader_set")!=shader_set||info.at("abi")!="ot2-vulkan-arm64-v1")throw std::runtime_error("seed identity mismatch");
            pipeline_seed_directory=seed;std::ifstream manifest(seed+"/manifest.jsonl");size_t imported=0;
            while(records.size()<8192&&std::getline(manifest,line)){
                if((bytes+=line.size())>pipeline_recipe_limit)break;
                try{if(line.size()>262144)throw std::runtime_error("seed recipe too large");auto envelope=Json::parse(line);auto& recipe=envelope.at("recipe");auto text=recipe.dump();if(envelope.at("checksum")!=content_key(text.data(),text.size()))throw std::runtime_error("seed checksum");
                    if(recorded_recipes.insert(text).second){records.push_back(recipe);++imported;}
                }catch(...){++warm_rejected;}
            }
            ot2_host_log("PIPELINE_SEED imported=%zu",imported);
        }
    }catch(const std::exception& e){pipeline_seed_directory.clear();ot2_host_log("PIPELINE_SEED_SKIP %s",e.what());}
    // Consolidate validated records once. Subsequent warm starts need only the
    // content-addressed directory; a torn optional write keeps the old file.
    {
        std::ofstream out(path+"/manifest.jsonl.tmp",std::ios::trunc);
        for(const auto& recipe:records){auto text=recipe.dump();Json envelope={{"recipe",recipe},{"checksum",content_key(text.data(),text.size())}};auto line=envelope.dump();
            if(line.size()+1>pipeline_recipe_limit-pipeline_recipe_bytes)break;out<<line<<'\n';pipeline_recipe_bytes+=line.size()+1;
        }
        out.close();if(out)std::filesystem::rename(path+"/manifest.jsonl.tmp",path+"/manifest.jsonl",error);
    }
    if(std::filesystem::exists(shader_directory+"/no-pipeline-warmup")||records.empty())return;
    warm_running=true;
    std::thread([records=std::move(records),migrate_bounds=dynamic_depth_bounds]{
        auto start=monotonic_ns();VkPipelineCache cache{};VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if(vkCreatePipelineCache(device,&pci,nullptr,&cache)!=VK_SUCCESS){{std::lock_guard lock(warm_done_mutex);warm_running=false;}warm_done.notify_all();return;}
        {std::lock_guard lock(driver_cache_mutex);vkMergePipelineCaches(device,cache,1,&pipeline_cache);}
        for(auto& json:records)try{
            PipelineRecipe recipe(json,migrate_bounds);auto key=recipe.key();
            request_pipeline(key,true,[&](PipelineWork& work){
                work.layout=pipeline_layout(recipe.bindings,recipe.pushes);VkPipeline pipeline{};
                auto result=recipe.with_info(work.layout->layout,[&](auto& c,auto& g){return recipe.compute?vkCreateComputePipelines(device,cache,1,&c,nullptr,&pipeline):create_graphics_pipeline(cache,g,&pipeline);});
                if(result!=VK_SUCCESS){if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);pipeline={};work.layout.reset();++warm_rejected;ot2_host_log("PIPELINE_WARM_REJECT result=%d compute=%d stages=%zu",result,recipe.compute,recipe.stages.size());}
                return pipeline;
            });
        }catch(const std::exception&){++warm_rejected;}
        {std::lock_guard lock(driver_cache_mutex);vkMergePipelineCaches(device,pipeline_cache,1,&cache);}
        vkDestroyPipelineCache(device,cache,nullptr);write_driver_cache();
        ot2_host_log("PIPELINE_WARMUP created=%llu rejected=%llu milliseconds=%.3f",(unsigned long long)warm_created.load(),(unsigned long long)warm_rejected.load(),(monotonic_ns()-start)/1e6);
        {std::lock_guard lock(warm_done_mutex);warm_running=false;}warm_done.notify_all();
    }).detach();
}
