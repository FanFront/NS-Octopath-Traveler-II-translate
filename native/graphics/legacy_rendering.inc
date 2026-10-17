// Vulkan 1.1 render-pass fallback. Pipeline recipes retain the common format
// description; only the final driver call differs. Render passes use a bounded
// cache and framebuffers stay leased until the command's actual fence retires.
bool dynamic_rendering=true;
struct LegacyPass {
    VkRenderPass handle{};
    ~LegacyPass(){if(handle)vkDestroyRenderPass(device,handle,nullptr);}
};
struct LegacyFramebuffer {
    std::shared_ptr<LegacyPass> pass;
    std::vector<VkImageView> views;
    VkFramebuffer handle{};
    uint64_t age{};
    ~LegacyFramebuffer(){if(handle)vkDestroyFramebuffer(device,handle,nullptr);}
};
std::mutex legacy_pass_mutex;
std::map<std::string,std::shared_ptr<LegacyPass>> legacy_passes;
std::map<std::string,std::shared_ptr<LegacyFramebuffer>> legacy_framebuffers;
std::map<VkCommandBuffer,std::vector<std::shared_ptr<LegacyFramebuffer>>> legacy_pending;
uint64_t legacy_framebuffer_age=0;
template<class T>void legacy_key_add(std::string& key,const T& value){key.append(reinterpret_cast<const char*>(&value),sizeof(value));}

std::shared_ptr<LegacyPass> legacy_pass(const VkPipelineRenderingCreateInfo& formats,VkSampleCountFlagBits samples,const VkRenderingInfo* render=nullptr,bool depth_input=false){
    if(formats.colorAttachmentCount>8||formats.viewMask)ot2_host_die("Unsupported legacy render-pass shape");
    std::array<VkAttachmentDescription,9> attachments{};
    std::array<VkAttachmentReference,8> colors{};
    VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
    uint32_t count=0;
    auto add=[&](VkFormat format,const VkRenderingAttachmentInfo* info,const VkRenderingAttachmentInfo* stencil){
        auto& a=attachments[count];a.format=format;a.samples=samples;
        a.loadOp=info?info->loadOp:VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp=info?info->storeOp:VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp=stencil?stencil->loadOp:VK_ATTACHMENT_LOAD_OP_LOAD;
        a.stencilStoreOp=stencil?stencil->storeOp:VK_ATTACHMENT_STORE_OP_STORE;
        // STORE_OP_NONE needs an extension on Vulkan 1.1. Preserving bytes is
        // the conservative fallback, including read-only depth/stencil passes.
        if(a.storeOp==VK_ATTACHMENT_STORE_OP_NONE)a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        if(a.stencilStoreOp==VK_ATTACHMENT_STORE_OP_NONE)a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout=a.finalLayout=info?info->imageLayout:VK_IMAGE_LAYOUT_GENERAL;
        return VkAttachmentReference{count++,a.initialLayout};
    };
    for(uint32_t i=0;i<formats.colorAttachmentCount;++i){
        colors[i]={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
        if(formats.pColorAttachmentFormats[i]!=VK_FORMAT_UNDEFINED)
            colors[i]=add(formats.pColorAttachmentFormats[i],render?&render->pColorAttachments[i]:nullptr,nullptr);
    }
    const auto depth_format=formats.depthAttachmentFormat!=VK_FORMAT_UNDEFINED?formats.depthAttachmentFormat:formats.stencilAttachmentFormat;
    if(depth_format!=VK_FORMAT_UNDEFINED){
        auto* d=render?render->pDepthAttachment:nullptr;auto* s=render?render->pStencilAttachment:nullptr;
        if(d&&s&&(d->imageView!=s->imageView||d->imageLayout!=s->imageLayout))ot2_host_die("Separate legacy depth/stencil attachments unsupported");
        depth=add(depth_format,d?d:s,s);
    }
    if(depth_input&&depth.attachment==VK_ATTACHMENT_UNUSED)ot2_host_die("Depth input requires a depth attachment");
    std::string key;legacy_key_add(key,formats.colorAttachmentCount);legacy_key_add(key,count);legacy_key_add(key,depth_input);
    for(uint32_t i=0;i<count;++i)legacy_key_add(key,attachments[i]);
    for(uint32_t i=0;i<formats.colorAttachmentCount;++i)legacy_key_add(key,colors[i]);legacy_key_add(key,depth);
    std::lock_guard lock(legacy_pass_mutex);
    if(auto found=legacy_passes.find(key);found!=legacy_passes.end())return found->second;
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=formats.colorAttachmentCount;subpass.pColorAttachments=colors.data();
    subpass.pDepthStencilAttachment=depth.attachment==VK_ATTACHMENT_UNUSED?nullptr:&depth;
    if(depth_input){subpass.inputAttachmentCount=1;subpass.pInputAttachments=&depth;}
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ci.attachmentCount=count;ci.pAttachments=attachments.data();ci.subpassCount=1;ci.pSubpasses=&subpass;
    auto result=std::make_shared<LegacyPass>();check(vkCreateRenderPass(device,&ci,nullptr,&result->handle),"legacy render pass");
    if(legacy_passes.size()>=1024){auto idle=std::find_if(legacy_passes.begin(),legacy_passes.end(),[](const auto& entry){return entry.second.use_count()==1;});if(idle!=legacy_passes.end())legacy_passes.erase(idle);}
    if(legacy_passes.size()<1024)legacy_passes.emplace(std::move(key),result);
    return result;
}

VkResult create_graphics_pipeline(VkPipelineCache cache,const VkGraphicsPipelineCreateInfo& info,VkPipeline* result){
    if(dynamic_rendering)return vkCreateGraphicsPipelines(device,cache,1,&info,nullptr,result);
    auto& formats=*static_cast<const VkPipelineRenderingCreateInfo*>(info.pNext);
    const bool depth_input=!depth_bounds_supported&&info.pDepthStencilState->depthBoundsTestEnable;
    auto pass=legacy_pass(formats,info.pMultisampleState->rasterizationSamples,nullptr,depth_input);
    auto legacy=info;legacy.pNext=nullptr;legacy.renderPass=pass->handle;legacy.subpass=0;
    auto depth=*info.pDepthStencilState;depth.depthBoundsTestEnable=depth_input?VK_FALSE:depth.depthBoundsTestEnable;legacy.pDepthStencilState=&depth;
    std::vector<VkDynamicState> states;
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    if(!depth_bounds_supported&&info.pDynamicState){for(uint32_t i=0;i<info.pDynamicState->dynamicStateCount;++i)if(info.pDynamicState->pDynamicStates[i]!=VK_DYNAMIC_STATE_DEPTH_BOUNDS)states.push_back(info.pDynamicState->pDynamicStates[i]);dynamic.dynamicStateCount=states.size();dynamic.pDynamicStates=states.data();legacy.pDynamicState=&dynamic;}
    return vkCreateGraphicsPipelines(device,cache,1,&legacy,nullptr,result);
}

void begin_rendering_compatible(VkCommandBuffer cmd,const VkRenderingInfo& render,const VkPipelineRenderingCreateInfo& formats,VkSampleCountFlagBits samples,bool depth_input=false){
    if(dynamic_rendering){vkCmdBeginRendering(cmd,&render);return;}
    if(render.flags||render.viewMask||render.renderArea.offset.x<0||render.renderArea.offset.y<0)ot2_host_die("Unsupported legacy rendering flags/area");
    auto pass=legacy_pass(formats,samples,&render,depth_input);
    std::vector<VkImageView> views;std::vector<VkClearValue> clears;
    for(uint32_t i=0;i<formats.colorAttachmentCount;++i)if(formats.pColorAttachmentFormats[i]!=VK_FORMAT_UNDEFINED){
        const auto& a=render.pColorAttachments[i];if(!a.imageView||a.resolveMode)ot2_host_die("Invalid legacy color attachment");views.push_back(a.imageView);clears.push_back(a.clearValue);
    }
    const auto* depth=render.pDepthAttachment?render.pDepthAttachment:render.pStencilAttachment;
    if(depth&&depth->imageView){if(depth->resolveMode)ot2_host_die("Legacy depth resolve pending");views.push_back(depth->imageView);auto value=depth->clearValue;if(render.pStencilAttachment)value.depthStencil.stencil=render.pStencilAttachment->clearValue.depthStencil.stencil;clears.push_back(value);}
    const uint32_t width=uint32_t(render.renderArea.offset.x)+render.renderArea.extent.width,height=uint32_t(render.renderArea.offset.y)+render.renderArea.extent.height;
    std::string key;legacy_key_add(key,pass->handle);legacy_key_add(key,width);legacy_key_add(key,height);legacy_key_add(key,render.layerCount);
    for(auto view:views)legacy_key_add(key,view);
    std::shared_ptr<LegacyFramebuffer> framebuffer;
    if(auto it=legacy_framebuffers.find(key);it!=legacy_framebuffers.end())framebuffer=it->second;
    else{
        framebuffer=std::make_shared<LegacyFramebuffer>();framebuffer->pass=pass;framebuffer->views=views;
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};ci.renderPass=pass->handle;ci.attachmentCount=views.size();ci.pAttachments=views.data();ci.width=width;ci.height=height;ci.layers=render.layerCount;
        check(vkCreateFramebuffer(device,&ci,nullptr,&framebuffer->handle),"legacy framebuffer");
        if(legacy_framebuffers.size()>=512){auto oldest=legacy_framebuffers.end();for(auto it=legacy_framebuffers.begin();it!=legacy_framebuffers.end();++it)if(it->second.use_count()==1&&(oldest==legacy_framebuffers.end()||it->second->age<oldest->second->age))oldest=it;if(oldest!=legacy_framebuffers.end())legacy_framebuffers.erase(oldest);}
        if(legacy_framebuffers.size()<512)legacy_framebuffers.emplace(std::move(key),framebuffer);
    }
    framebuffer->age=++legacy_framebuffer_age;legacy_pending[cmd].push_back(framebuffer);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass->handle;begin.framebuffer=framebuffer->handle;begin.renderArea=render.renderArea;begin.clearValueCount=clears.size();begin.pClearValues=clears.data();
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
}
void end_rendering_compatible(VkCommandBuffer cmd){if(dynamic_rendering)vkCmdEndRendering(cmd);else vkCmdEndRenderPass(cmd);}
void retire_legacy_rendering(VkCommandBuffer cmd){if(!dynamic_rendering)legacy_pending.erase(cmd);}
void legacy_forget_view(VkImageView view){
    if(dynamic_rendering)return;
    for(auto it=legacy_framebuffers.begin();it!=legacy_framebuffers.end();){auto& views=it->second->views;if(std::find(views.begin(),views.end(),view)!=views.end())it=legacy_framebuffers.erase(it);else ++it;}
}
