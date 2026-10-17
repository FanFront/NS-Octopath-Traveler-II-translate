// A small independent layered render, plus optional locally captured game LUT
// constants/shader. No proprietary data is embedded in the source or APK.
extern "C" int vkhost_test_layered_render(const char* directory,const char* test_case){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=true;
    const std::string name=test_case;const bool volume=name.ends_with("3d"),simple=name.starts_with("simple"),captured=name.find("captured")!=std::string::npos,real_vertex=captured||name.find("realvs")!=std::string::npos,real_geometry=real_vertex||name.find("realgs")!=std::string::npos;
    if(!simple&&!name.starts_with("game")&&!name.starts_with("opt"))return 0;
    auto load=[&](const std::string& file){std::ifstream in(std::string(directory)+"/"+file,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Missing layered fixture "+file);const size_t n=in.tellg();if(n>1024*1024)throw std::runtime_error("Layered fixture too large");std::vector<uint8_t> bytes(n);in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),n);if(!in)throw std::runtime_error("Short layered fixture");return bytes;};
    const bool discard_pixels=name.find("discard")!=std::string::npos,odd=name.find("odd")!=std::string::npos;
    VkShaderModule fragment=discard_pixels?clear_module(sample_count_probe_frag):clear_module(layered_probe_frag);
    if(!simple){auto bytes=load(name.starts_with("opt")?"optimized.spv":"original.spv");if(bytes.size()%4)return 0;std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());fragment=shared_shader_module(words.data(),words.size());}
    tex_t tex{};tex.magic=MAGIC('T','E','X',0);tex.serial=nvn_new_serial();tex.d.width=tex.d.height=32;tex.d.depth=volume?32:1;tex.d.levels=1;tex.d.samples=1;tex.d.target=volume?NVN_TEXTURE_TARGET_3D:NVN_TEXTURE_TARGET_2D;tex.d.format=NVN_FORMAT_RGB10A2;
    for(unsigned i=0;i<4;i++)tex.d.swizzle[i]=i+2;nvn_tex_layout(&tex.d,&tex.L);auto& img=image_for(tex);
    Image* depth_image=nullptr;VkImageView depth_view{};
    if(name.find("depth")!=std::string::npos){auto depth_tex=tex;depth_tex.serial=nvn_new_serial();depth_tex.d.target=volume?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D;depth_tex.d.format=NVN_FORMAT_DEPTH32F;nvn_tex_layout(&depth_tex.d,&depth_tex.L);depth_image=&image_for(depth_tex);depth_view=make_view(depth_tex,nullptr,true);}
    auto resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    if(!simple)for(unsigned i=0;i<3;i++){auto bytes=load((captured?"actual-c":"c")+std::to_string(i==0?1:i+2)+".bin");resources->buffer(513+i,bytes.data(),bytes.size(),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);}
    if(real_vertex){resources->current_stage=VK_SHADER_STAGE_VERTEX_BIT;for(unsigned i=0;i<2;i++){auto bytes=load("vc"+std::to_string(4-i)+".bin");resources->buffer(1+i,bytes.data(),bytes.size(),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);}}
    const bool soft_query=name.find("softquery")!=std::string::npos;GpuBuffer sample_buffer{};
    const uint64_t initial_count=name.find("overflow")!=std::string::npos?(uint64_t(7)<<32)+UINT32_MAX-511:0;
    if(soft_query){sample_buffer=make_buffer(&initial_count,8,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);resources->current_stage=VK_SHADER_STAGE_FRAGMENT_BIT;resources->adopt(software_samples_binding,sample_buffer,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);fragment=software_samples_module(fragment);}
    resources->finish();
    VkPipelineShaderStageCreateInfo stages[3]{};auto geometry=clear_module(layered_probe_geom);if(real_geometry){auto bytes=load(name.find("nolayer")!=std::string::npos?"geometry-nolayer.spv":"geometry.spv");std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());geometry=shared_shader_module(words.data(),words.size());}
    auto vertex=clear_module(layered_probe_vert);if(real_vertex){auto bytes=load("vertex.spv");std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());vertex=shared_shader_module(words.data(),words.size());}
    const VkShaderModule modules[]={vertex,geometry,fragment};const VkShaderStageFlagBits kinds[]={VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_GEOMETRY_BIT,VK_SHADER_STAGE_FRAGMENT_BIT};
    for(unsigned i=0;i<3;i++){stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[i].stage=kinds[i];stages[i].module=modules[i];stages[i].pName="main";}
    VkPipelineRenderingCreateInfo formats{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};formats.colorAttachmentCount=1;formats.pColorAttachmentFormats=&img.format;if(depth_image)formats.depthAttachmentFormat=depth_image->format;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkVertexInputAttributeDescription attrs[]={{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32A32_SFLOAT,16}};VkVertexInputBindingDescription binding{0,32,VK_VERTEX_INPUT_RATE_VERTEX};if(real_vertex){vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;vi.vertexAttributeDescriptionCount=2;vi.pVertexAttributeDescriptions=attrs;if(captured){binding.stride=16;attrs[0].format=attrs[1].format=VK_FORMAT_R32G32_SFLOAT;attrs[1].offset=8;}}VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=captured?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=vs.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
    VkDynamicState dyns[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=dyns;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&formats;ci.stageCount=3;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vs;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.pDynamicState=&dyn;ci.layout=resources->layout;
    if(name.find("nogs")!=std::string::npos){stages[1]=stages[2];ci.stageCount=2;}
    if(name.find("recipe")!=std::string::npos){auto bytes=load("pipeline.json");auto recipe=Json::parse(bytes.begin(),bytes.end());
        rs=recipe_value<VkPipelineRasterizationStateCreateInfo>(recipe.at("raster"));ds=recipe_value<VkPipelineDepthStencilStateCreateInfo>(recipe.at("depth"));
        ms=recipe_value<VkPipelineMultisampleStateCreateInfo>(recipe.at("multisample"));ms.pSampleMask=nullptr;
        auto attachments=recipe_values<VkPipelineColorBlendAttachmentState>(recipe.at("blends"),8);if(attachments.size()!=1)return 0;blend=attachments[0];
        cb=recipe_value<VkPipelineColorBlendStateCreateInfo>(recipe.at("blend"));cb.pAttachments=&blend;
    }
    auto pipeline=cached_pipeline(ci,resources->bindings);auto view=make_view(tex,nullptr,true);auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=view;color.imageLayout=img.layout;color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea.extent={32,32};render.layerCount=volume?32:1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
    VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};if(depth_image){barrier(cmd,*depth_image,depth_image->layout,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);depth_attachment.imageView=depth_view;depth_attachment.imageLayout=depth_image->layout;depth_attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;depth_attachment.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;render.pDepthAttachment=&depth_attachment;}
    const bool query=!soft_query&&name.find("query")!=std::string::npos,outer_query=name.find("outerquery")!=std::string::npos;const VkQueryPool sample_pool=occlusion_query;const VkQueryControlFlags query_flags=precise_occlusion&&name.find("imprecise")==std::string::npos?VK_QUERY_CONTROL_PRECISE_BIT:0;
    if(outer_query)vkCmdBeginQuery(cmd,occlusion_query,0,query_flags);
    begin_rendering_compatible(cmd,render,formats,VK_SAMPLE_COUNT_1_BIT);
    VkViewport viewport=captured?VkViewport{0,32,32,-32,0,0}:VkViewport{0,0,32,32,0,1};VkRect2D scissor=odd?VkRect2D{{1,2},{29,27}}:VkRect2D{{0,0},{32,32}};vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,resources->layout,resources->set);
    decltype(make_buffer(nullptr,1,0)) vertex_buffer{};if(real_vertex){const float data[]={-1,-1,0,1,0,0,0,0,3,-1,0,1,2,0,0,0,-1,3,0,1,0,2,0,0};if(captured){auto bytes=load("actual-v0.bin");vertex_buffer=make_buffer(bytes.data(),bytes.size(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);}else vertex_buffer=make_buffer(data,sizeof(data),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);vkCmdBindVertexBuffers(cmd,0,1,&vertex_buffer.buffer,&vertex_buffer.offset);}
    if(query&&!outer_query)vkCmdBeginQuery(cmd,occlusion_query,0,query_flags);
    vkCmdDraw(cmd,captured?4:3,volume?32:1,0,0);
    if(query&&!outer_query)vkCmdEndQuery(cmd,occlusion_query,0);
    end_rendering_compatible(cmd);
    if(outer_query)vkCmdEndQuery(cmd,occlusion_query,0);if(query)pending_queries=1;
    const size_t pixels=32*32*(volume?32:1);auto result=make_buffer(nullptr,pixels*4+16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,true);memset(result.mapped,0xcc,result.size);
    barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy copy{};copy.bufferOffset=result.offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={32,32,volume?32u:1u};vkCmdCopyImageToBuffer(cmd,img.image,img.layout,result.buffer,1,&copy);finish_commands(cmd);flush_commands();
    bool valid=true;uint64_t counted=0;if(soft_query)memcpy(&counted,sample_buffer.mapped,8);else if(query)check(vkGetQueryPoolResults(device,sample_pool,0,1,8,&counted,8,VK_QUERY_RESULT_64_BIT),"layered query read");
    size_t surviving=0;for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++)if((!discard_pixels||x>=16)&&(!odd||(x>=1&&x<30&&y>=2&&y<29)))++surviving;surviving*=volume?32:1;
    if(soft_query||query){valid&=counted==initial_count+surviving;printf("LAYERED_SAMPLES actual=%llu expected=%llu\n",(unsigned long long)counted,(unsigned long long)(initial_count+surviving));}
    auto* data=static_cast<uint32_t*>(result.mapped);for(unsigned i=0;i<4;i++)valid&=data[pixels+i]==0xcccccccc;
    if(simple)for(size_t i=0;i<pixels;i++){unsigned x=i%32,y=(i/32)%32,z=i/1024;uint32_t value=data[i];if((discard_pixels&&x<16)||(odd&&(x<1||x>=30||y<2||y>=29))){valid&=value==0;continue;}valid&=std::abs(int(value&1023)-int(std::lround(z/32.f*1023)))<=1;valid&=std::abs(int((value>>10)&1023)-int(std::lround((x+.5f)/32*1023)))<=1;valid&=std::abs(int((value>>20)&1023)-int(std::lround((y+.5f)/32*1023)))<=1;valid&=(value>>30)==3;}
    std::ofstream(std::string(directory)+"/"+name+".raw",std::ios::binary).write(reinterpret_cast<const char*>(data),pixels*4);
    printf("LAYERED_RENDER case=%s valid=%u bytes=%zu\n",test_case,valid,pixels*4);resources.reset();destroy_buffer(result);if(vertex_buffer.buffer)destroy_buffer(vertex_buffer);batching=false;return valid;
}
