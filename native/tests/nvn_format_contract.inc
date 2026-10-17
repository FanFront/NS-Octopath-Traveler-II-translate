// Small real-image sampling oracle for format-dependent filtering support.
extern "C" int vkhost_test_texel_formats(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=false;bool valid=true;
    for(auto format:{VK_FORMAT_R16_UINT,VK_FORMAT_R32_UINT})for(unsigned mode:{0,1,2}){
        std::array<uint32_t,4> source32{1,37,65535,0};std::array<uint16_t,4> source16{1,37,65535,0};std::array<uint32_t,8> output;output.fill(0xcccccccc);
        auto input=make_buffer(format==VK_FORMAT_R16_UINT?static_cast<void*>(source16.data()):static_cast<void*>(source32.data()),format==VK_FORMAT_R16_UINT?8:16,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
        VkBufferViewCreateInfo vi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};vi.buffer=input.buffer;vi.format=format;vi.offset=input.offset;vi.range=input.size;VkBufferView view{};check(vkCreateBufferView(device,&vi,nullptr,&view),"format oracle texel view");
        auto resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;resources->buffer(2,output.data(),16,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,true);
        resources->bindings.push_back({1,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});resources->writes.push_back({1,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,0,2});resources->texel_views.push_back(view);resources->texel_view_leases.push_back(nullptr);resources->finish();
        auto module=mode==1?clear_module(texel_view_probe_comp):clear_module(format_texel_probe_comp);
        if(mode==2){const ShaderIntegerImage f{1,0,format==VK_FORMAT_R16_UINT?uint32_t(spv::ImageFormatR16ui):uint32_t(spv::ImageFormatR32ui)};auto words=shader_integer_images(format_texel_probe_comp,{&f,1});module=shared_shader_module(words.data(),words.size());}
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=resources->layout;
        auto pipeline=cached_pipeline(ci,resources->bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set);vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);flush_commands();resources.reset();
        bool pass=true;for(unsigned i=0;i<4;i++)pass&=output[i]==source32[i]+17;for(unsigned i=4;i<8;i++)pass&=output[i]==0xcccccccc;if(mode||getenv("OT2_TEXEL_FORMATS_NEGATIVE"))valid&=pass;
        printf("TEXEL_FORMAT vk=%d mode=%u actual=%u,%u,%u,%u pass=%u\n",format,mode,output[0],output[1],output[2],output[3],pass);destroy_buffer(input);
    }return valid;
}

extern "C" int vkhost_test_storage_formats(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=false;bool valid=true;
    for(auto format:{VK_FORMAT_R16_UINT,VK_FORMAT_R32_UINT}){
        std::array<uint32_t,4> source32{1,37,65532,0};std::array<uint16_t,4> source16{1,37,65532,0};std::array<uint32_t,8> output;output.fill(0xcccccccc);
        auto input=make_buffer(format==VK_FORMAT_R16_UINT?static_cast<void*>(source16.data()):static_cast<void*>(source32.data()),format==VK_FORMAT_R16_UINT?8:16,VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
        VkBufferViewCreateInfo vi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};vi.buffer=input.buffer;vi.format=format;vi.offset=input.offset;vi.range=input.size;VkBufferView view{};check(vkCreateBufferView(device,&vi,nullptr,&view),"format oracle storage texel view");
        auto resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;resources->buffer(2,output.data(),16,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,true);
        resources->bindings.push_back({1,VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});resources->writes.push_back({1,VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,0,2});resources->texel_views.push_back(view);resources->texel_view_leases.push_back(nullptr);resources->finish();
        const ShaderIntegerImage f{1,0,format==VK_FORMAT_R16_UINT?uint32_t(spv::ImageFormatR16ui):uint32_t(spv::ImageFormatR32ui)};auto words=shader_integer_images(format_storage_probe_comp,{&f,1});
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shared_shader_module(words.data(),words.size());ci.stage.pName="main";ci.layout=resources->layout;
        auto pipeline=cached_pipeline(ci,resources->bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set);vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);flush_commands();resources.reset();
        bool pass=true;for(unsigned i=0;i<4;i++){pass&=output[i]==source32[i];auto written=format==VK_FORMAT_R16_UINT?static_cast<uint16_t*>(input.mapped)[i]:static_cast<uint32_t*>(input.mapped)[i];pass&=written==((source32[i]+3)&(format==VK_FORMAT_R16_UINT?65535:UINT32_MAX));}for(unsigned i=4;i<8;i++)pass&=output[i]==0xcccccccc;valid&=pass;
        printf("STORAGE_FORMAT vk=%d actual=%u,%u,%u,%u pass=%u\n",format,output[0],output[1],output[2],output[3],pass);destroy_buffer(input);
    }return valid;
}

extern "C" int vkhost_test_filter_formats(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=false;
    bool valid=true;
    for(int f:{NVN_FORMAT_RG16,NVN_FORMAT_RG16F,NVN_FORMAT_RGBA8}){
        tex_t tex{};tex.magic=MAGIC('T','E','X',0);tex.serial=nvn_new_serial();tex.d.width=tex.d.height=2;tex.d.depth=tex.d.levels=tex.d.samples=1;tex.d.target=NVN_TEXTURE_TARGET_2D;tex.d.format=f;
        for(unsigned i=0;i<4;i++)tex.d.swizzle[i]=i+2;nvn_tex_layout(&tex.d,&tex.L);
        std::array<uint16_t,8> normalized{0,0,65535,0,0,65535,65535,65535};
        std::array<uint16_t,8> floating{0,0,0x3c00,0,0,0x3c00,0x3c00,0x3c00};
        std::array<uint8_t,16> rgba{0,0,0,255,255,0,0,255,0,255,0,255,255,255,0,255};
        p_copy_b2t upload{};upload.tex=&tex;upload.r={0,0,0,2,2,1};upload.src=reinterpret_cast<uintptr_t>(f==NVN_FORMAT_RG16?static_cast<void*>(normalized.data()):f==NVN_FORMAT_RG16F?static_cast<void*>(floating.data()):static_cast<void*>(rgba.data()));copy_to_texture(upload);
        auto& img=image_for(tex);VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(physical,img.format,&properties);
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_LINEAR;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler{};check(vkCreateSampler(device,&sc,nullptr,&sampler),"format oracle sampler");
        std::array<float,8> output;output.fill(-12345);
        auto resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;resources->buffer(2,output.data(),16,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,true);
        resources->bindings.push_back({1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});resources->writes.push_back({1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,0,1});resources->image_infos.push_back({sampler,make_view(tex,nullptr),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});resources->finish();
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=clear_module(format_probe_comp);ci.stage.pName="main";ci.layout=resources->layout;
        auto pipeline=cached_pipeline(ci,resources->bindings);auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set);vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);flush_commands();resources.reset();
        bool pass=std::abs(output[0]-.5f)<.003f&&std::abs(output[1]-.5f)<.003f&&output[2]==0&&output[3]==1;for(unsigned i=4;i<8;i++)pass&=output[i]==-12345;valid&=pass;
        printf("FILTER_FORMAT nvn=%d vk=%d supported=%u rgba=%g,%g,%g,%g pass=%u\n",f,img.format,bool(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),output[0],output[1],output[2],output[3],pass);
        vkDestroySampler(device,sampler,nullptr);destroy_image(tex.serial);
    }
    return valid;
}

// Exercise the display encoder itself, not an unrelated sRGB render target.
extern "C" int vkhost_test_present_formats(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=false;bool valid=true;
    auto texture=[](int f,unsigned w,unsigned h){tex_t t{};t.magic=MAGIC('T','E','X',0);t.serial=nvn_new_serial();t.d.width=w;t.d.height=h;t.d.depth=t.d.levels=t.d.samples=1;t.d.target=NVN_TEXTURE_TARGET_2D;t.d.format=f;for(unsigned i=0;i<4;i++)t.d.swizzle[i]=i+2;nvn_tex_layout(&t.d,&t.L);return t;};
    for(int f:{NVN_FORMAT_RGBA8,NVN_FORMAT_RGBA8_SRGB,NVN_FORMAT_BGRA8_SRGB})for(int target_format:{NVN_FORMAT_RGBA8,NVN_FORMAT_BGRA8})for(int mode:{0,1,2}){
        unsigned ow=mode==0?8:mode==1?13:3,oh=mode==0?4:mode==1?13:2;
        int crop[4]{};if(mode==1){crop[0]=2;crop[1]=1;crop[2]=4;crop[3]=2;}
        int sx=crop[0],sy=crop[1],sw=crop[2]?crop[2]:8,sh=crop[3]?crop[3]:4;
        auto src=texture(f,8,4),dst=texture(target_format,ow,oh);
        const uint8_t grey[]{0,16,32,64,96,128,192,255};std::array<uint8_t,128> input{};for(unsigned i=0;i<32;i++){input[i*4]=grey[i%8];input[i*4+1]=grey[(i+1)%8];input[i*4+2]=grey[(i+2)%8];input[i*4+3]=37+i;}
        p_copy_b2t copy{};copy.tex=&src;copy.r={0,0,0,8,4,1};copy.src=reinterpret_cast<uintptr_t>(input.data());copy_to_texture(copy);
        auto& out=image_for(dst);auto cmd=begin_commands();encode_present_pixels(cmd,src,out,make_view(dst,nullptr,true),{ow,oh},crop);finish_commands(cmd);
        std::vector<uint8_t> output(ow*oh*4+16,0xcc);copy.tex=&dst;copy.r={0,0,0,int(ow),int(oh),1};copy.src=reinterpret_cast<uintptr_t>(output.data());copy_from_texture(copy,0,0);
        double scale=std::min(double(ow)/sw,double(oh)/sh);int dw=int(sw*scale),dh=int(sh*scale),dx=(ow-dw)/2,dy=(oh-dh)/2;
        unsigned max_error=0;for(unsigned y=0;y<oh;y++)for(unsigned x=0;x<ow;x++)for(unsigned c=0;c<4;c++){
            int expected=0;
            if(int(x)>=dx&&int(x)<dx+dw&&int(y)>=dy&&int(y)<dy+dh){
                double fx=std::clamp(sx+(x-dx+.5)*sw/dw-.5,double(sx),double(sx+sw-1)),fy=std::clamp(sy+(y-dy+.5)*sh/dh-.5,double(sy),double(sy+sh-1));
                int x0=int(fx),y0=int(fy),x1=std::min(x0+1,sx+sw-1),y1=std::min(y0+1,sy+sh-1);double a=fx-x0,b=fy-y0;
                unsigned channel=((f==NVN_FORMAT_BGRA8_SRGB)!=(target_format==NVN_FORMAT_BGRA8))&&c<3?2-c:c;
                auto v=[&](int xx,int yy){return input[(yy*8+xx)*4+channel];};
                expected=int(std::lround((v(x0,y0)*(1-a)+v(x1,y0)*a)*(1-b)+(v(x0,y1)*(1-a)+v(x1,y1)*a)*b));
            }
            max_error=std::max(max_error,unsigned(std::abs(int(output[(y*ow+x)*4+c])-expected)));
        }
        bool pass=max_error<=1;for(unsigned i=ow*oh*4;i<output.size();i++)pass&=output[i]==0xcc;valid&=pass;
        printf("PRESENT_FORMAT nvn=%d target=%d mode=%d max_error=%u pass=%u\n",f,target_format,mode,max_error,pass);destroy_image(src.serial);destroy_image(dst.serial);
    }return valid;
}
