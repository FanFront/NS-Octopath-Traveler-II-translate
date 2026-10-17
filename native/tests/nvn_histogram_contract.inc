// Optional real-game compute fixture supplied locally, never packaged in the
// repository/APK. Uniform grey images have an independently known histogram.
extern "C" int vkhost_test_histogram(const char* shader_path){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();batching=false;
    std::ifstream in(shader_path,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Missing histogram fixture");
    size_t bytes=in.tellg();if(!bytes||bytes%4||bytes>1024*1024)throw std::runtime_error("Invalid histogram fixture");
    std::vector<uint32_t> words(bytes/4);in.seekg(0);in.read(reinterpret_cast<char*>(words.data()),bytes);if(!in)throw std::runtime_error("Short histogram fixture");
    auto module=shared_shader_module(words.data(),words.size());
    auto texture=[](int format,int width,int height){tex_t t{};t.magic=MAGIC('T','E','X',0);t.serial=nvn_new_serial();t.d.width=width;t.d.height=height;t.d.depth=t.d.levels=t.d.samples=1;t.d.target=NVN_TEXTURE_TARGET_2D;t.d.format=format;for(unsigned i=0;i<4;i++)t.d.swizzle[i]=i+2;nvn_tex_layout(&t.d,&t.L);return t;};
    auto half=[](uint16_t h){return std::ldexp(float((h&1023)+((h&0x7c00)?1024:0)),((h>>10)&31)?int((h>>10)&31)-25:-24)*(h&0x8000?-1:1);};
    bool valid=true;
    for(unsigned groups:{1u,56u})for(float grey:{.0625f,1.f,16.f}){
        unsigned gx=groups==1?1:7,gy=groups/gx,width=gx*64,height=gy*32;
        auto scene=texture(NVN_FORMAT_RGBA32F,width,height),meter=texture(NVN_FORMAT_RGBA32F,1,1),output=texture(NVN_FORMAT_RGBA16F,16,groups);
        std::vector<float> pixels(width*height*4);for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){unsigned group=y/32*gx+x/64;float value=grey*std::exp2(int(group%9)-4);for(unsigned c=0;c<4;c++)pixels[(y*width+x)*4+c]=value;}std::array<float,4> white{1,1,1,1};
        p_copy_b2t copy{};copy.tex=&scene;copy.r={0,0,0,int(width),int(height),1};copy.src=reinterpret_cast<uintptr_t>(pixels.data());copy_to_texture(copy);
        copy.tex=&meter;copy.r={0,0,0,1,1,1};copy.src=reinterpret_cast<uintptr_t>(white.data());copy_to_texture(copy);
        // The layout/bindings belong to the supplied histogram shader. c4
        // uses integer bit patterns, c3/c5 floating point constants.
        std::array<uint32_t,16384> c4{};c4[4]=width;c4[5]=height;c4[8]=gx;
        std::array<float,16384> c5{},c3{},c1{};c5[0]=1.f/16;c5[4]=.5f;c5[8]=.0001f;c5[12]=1;c5[16]=width;c5[17]=height;c5[20]=1.f/width;c5[21]=1.f/height;c3[135*4+2]=1;c1[0]=.3333333134651184f;
        auto resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;
        resources->buffer(644,c4.data(),sizeof(c4),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);resources->buffer(645,c5.data(),sizeof(c5),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);resources->buffer(646,c3.data(),sizeof(c3),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);resources->buffer(647,c1.data(),sizeof(c1),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_NEAREST;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler{};check(vkCreateSampler(device,&sc,nullptr,&sampler),"histogram sampler");
        unsigned binding=641;for(auto* tex:{&scene,&meter,&output}){bool storage=tex==&output;auto type=storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;resources->bindings.push_back({binding,type,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});resources->writes.push_back({binding,type,resources->image_infos.size(),1});resources->image_infos.push_back({storage?VK_NULL_HANDLE:sampler,make_view(*tex,nullptr),storage?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});++binding;}
        resources->finish();VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=resources->layout;
        auto pipeline=cached_pipeline(ci,resources->bindings);auto cmd=begin_commands();for(auto* tex:{&scene,&meter,&output}){auto& img=image_for(*tex);barrier(cmd,img,img.layout,tex==&output?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
        bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set);vkCmdDispatch(cmd,gx,gy,1);finish_commands(cmd);flush_commands();resources.reset();
        std::vector<uint16_t> data(groups*64+8,0xcccc);copy.tex=&output;copy.r={0,0,0,16,int(groups),1};copy.src=reinterpret_cast<uintptr_t>(data.data());copy_from_texture(copy,0,0);
        double max_error=0,total=0;
        for(unsigned group=0;group<groups;group++){
            double position=std::clamp((std::log2(grey)+int(group%9)-4)/16+.5,0.,1.)*63;int bin=int(position);double weight=position-bin;
            for(int i=0;i<64;i++){double expected=(i==bin?1-weight:i==bin+1?weight:0)/groups,actual=half(data[group*64+i]);max_error=std::max(max_error,std::abs(actual-expected));total+=actual;}
        }
        bool pass=max_error<.002/groups;for(unsigned i=groups*64;i<data.size();i++)pass&=data[i]==0xcccc;valid&=pass;
        printf("HISTOGRAM groups=%u grey=%g total=%g max_error=%g pass=%u\n",groups,grey,total,max_error,pass);
        vkDestroySampler(device,sampler,nullptr);destroy_image(scene.serial);destroy_image(meter.serial);destroy_image(output.serial);
    }return valid;
}
