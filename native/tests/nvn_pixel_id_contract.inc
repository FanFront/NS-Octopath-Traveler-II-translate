// Synthetic Maxwell PIXLD -> SPIR-V -> real raster samples -> independent GLSL
// readback. This catches a fake zero implementation even if compilation succeeds.
extern "C" int vkhost_test_pixel_id(const char* directory){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=false;
    auto load=[&](const char* file){std::ifstream in(std::string(directory)+"/"+file,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Missing pixel-id fixture");auto size=in.tellg();if(size<20||size>1024*1024||size%4)throw std::runtime_error("Invalid pixel-id fixture");std::vector<uint32_t> words(size_t(size)/4);in.seekg(0);in.read(reinterpret_cast<char*>(words.data()),size);if(!in)throw std::runtime_error("Short pixel-id fixture");return shared_shader_module(words.data(),words.size());};
    auto fragment=load("pixel-id.spv"),vertex=clear_module(layered_probe_vert);
    VkImageFormatProperties support{};check(vkGetPhysicalDeviceImageFormatProperties(physical,VK_FORMAT_R32_UINT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&support),"sample-id format");
    bool valid=true;unsigned multi_cases=0;
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_NEAREST;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};check(vkCreateSampler(device,&sc,nullptr,&sampler),"sample-id sampler");
    for(unsigned samples:{1u,2u,4u,8u}){
        if(!(support.sampleCounts&samples)){printf("PIXLD samples=%u unsupported\n",samples);continue;}
        multi_cases+=samples>1;
        tex_t tex{};tex.magic=MAGIC('T','E','X',0);tex.serial=nvn_new_serial();tex.d.width=tex.d.height=32;tex.d.depth=tex.d.levels=1;tex.d.samples=samples;tex.d.target=NVN_TEXTURE_TARGET_2D;tex.d.format=NVN_FORMAT_R32UI;
        for(unsigned i=0;i<4;i++)tex.d.swizzle[i]=i+2;nvn_tex_layout(&tex.d,&tex.L);auto& img=image_for(tex);auto view=make_view(tex,nullptr,true);
        auto resources=std::make_unique<DrawResources>();resources->finish();
        VkPipelineShaderStageCreateInfo stages[2]{};for(unsigned i=0;i<2;i++){stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?fragment:vertex;stages[i].pName="main";}
        VkPipelineRenderingCreateInfo formats{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};formats.colorAttachmentCount=1;formats.pColorAttachmentFormats=&img.format;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=vs.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
        // SampleId itself requires sample-rate shading; no global forced setting.
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VkSampleCountFlagBits(samples);
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=1;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
        VkDynamicState dyns[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=dyns;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&formats;ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vs;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.pDynamicState=&dyn;ci.layout=resources->layout;
        auto pipeline=cached_pipeline(ci,resources->bindings);auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=view;color.imageLayout=img.layout;color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;color.clearValue.color.uint32[0]=0xdddddddd;
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea.extent={32,32};render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
        begin_rendering_compatible(cmd,render,formats,VkSampleCountFlagBits(samples));VkViewport viewport{0,0,32,32,0,1};VkRect2D scissor{{0,0},{32,32}};vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,resources->layout,resources->set);vkCmdDraw(cmd,3,1,0,0);end_rendering_compatible(cmd);finish_commands(cmd);flush_commands();resources.reset();
        const size_t count=32*32*samples;auto output=make_buffer(nullptr,(count+4)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0,true);memset(output.mapped,0xcc,output.size);
        resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;resources->adopt(2,output,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        resources->bindings.push_back({1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});resources->writes.push_back({1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,0,1});resources->image_infos.push_back({sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});resources->finish();
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=load(samples>1?"read-ms.spv":"read-single.spv");compute.stage.pName="main";compute.layout=resources->layout;
        auto read_pipeline=cached_pipeline(compute,resources->bindings);cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,read_pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set);vkCmdDispatch(cmd,4,4,1);finish_commands(cmd);flush_commands();
        auto* data=static_cast<const uint32_t*>(output.mapped);bool pass=true;for(size_t i=0;i<count;i++)pass&=data[i]==i%samples;for(size_t i=count;i<count+4;i++)pass&=data[i]==0xcccccccc;
        printf("PIXLD samples=%u pixels=1024 first=%u,%u,%u,%u pass=%u\n",samples,data[0],data[1],data[2],data[3],pass);valid&=pass;resources.reset();destroy_image(tex.serial);
    }
    vkDestroySampler(device,sampler,nullptr);return valid&&multi_cases>0;
}
