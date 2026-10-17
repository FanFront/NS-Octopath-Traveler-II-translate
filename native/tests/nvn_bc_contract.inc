// Both software decode and (where available) native BC hardware are sampled by
// the same compute shader. Transfers/readbacks retain the compressed byte ABI.
static std::vector<float> bc_sample(const tex_t& texture,unsigned level){
    auto& img=image_for(texture);view_t view{};view.set_levels=1;view.base_level=level;view.nlevels=1;
    const unsigned w=std::max(1U,texture.d.width>>level),h=std::max(1U,texture.d.height>>level),z=img.is_3d?std::max(1U,texture.d.depth>>level):img.layers;
    std::vector<float> output(size_t(w)*h*z*4+4,-12345);
    auto resources=std::make_unique<DrawResources>();resources->current_stage=VK_SHADER_STAGE_COMPUTE_BIT;
    resources->buffer(1,output.data(),(output.size()-4)*sizeof(float),VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,true);
    auto sampled=make_view(texture,&view);resources->bindings.push_back({2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    resources->writes.push_back({2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,0,true});resources->image_infos.push_back({default_sampler(),sampled,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});resources->finish();
    static VkShaderModule array_module{},volume_module{};if(!array_module){array_module=clear_module(bc_array_probe_comp);volume_module=clear_module(bc_volume_probe_comp);}
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=img.is_3d?volume_module:array_module;ci.stage.pName="main";ci.layout=resources->layout;
    auto pipeline=cached_pipeline(ci,resources->bindings);auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set,resources->dynamic_offsets);vkCmdDispatch(cmd,w,h,z);finish_commands(cmd);flush_commands();resources.reset();
    for(size_t i=output.size()-4;i<output.size();i++)if(output[i]!=-12345)ot2_host_die("BC sample guard");
    return output;
}
extern "C" int vkhost_test_bc(){
    drain_queues();std::lock_guard lock(state_mutex);const bool hardware=native_bc,old_batching=batching;batching=false;
    auto old_row=copy_row_stride,old_layer=copy_image_stride;copy_row_stride=copy_image_stride=0;
    const auto images_before=images.size(),views_before=image_views.size();unsigned cases=0;
    for(int f=NVN_FORMAT_RGB_DXT1;f<=NVN_FORMAT_BPTC_UFLOAT;f++)for(bool volume:{false,true}){
        tex_t soft{};soft.magic=MAGIC('T','E','X',0);soft.serial=nvn_new_serial();soft.d={};soft.d.width=7;soft.d.height=5;soft.d.depth=3;soft.d.levels=3;soft.d.format=f;soft.d.target=volume?NVN_TEXTURE_TARGET_3D:NVN_TEXTURE_TARGET_2D_ARRAY;
        for(unsigned i=0;i<4;i++)soft.d.swizzle[i]=i+2;nvn_tex_layout(&soft.d,&soft.L);
        auto info=bc_format(f);std::vector<uint8_t> storage(soft.L.size);soft.mem=storage.data();
        std::vector<std::vector<uint8_t>> expected;
        for(unsigned level=0;level<3;level++){
            unsigned w=std::max(1U,soft.d.width>>level),h=std::max(1U,soft.d.height>>level),z=volume?std::max(1U,soft.d.depth>>level):3;
            size_t row=((w+3)/4)*info.block_bytes,rows=(h+3)/4;expected.emplace_back(row*rows*z);auto& data=expected.back();
            for(size_t i=0;i<data.size();i++)data[i]=uint8_t(i*19+level*7+11);
            for(size_t off=0;off<data.size();off+=info.block_bytes){
                if(f==NVN_FORMAT_BPTC_UNORM||f==NVN_FORMAT_BPTC_UNORM_SRGB)data[off]=0x40; // valid BC7 mode 6
                if(f==NVN_FORMAT_BPTC_SFLOAT||f==NVN_FORMAT_BPTC_UFLOAT)data[off]=(data[off]&0xe0)|3; // valid BC6 mode 11
            }
            for(unsigned layer=0;layer<(volume?1:3);layer++)if(nvn_swizzle(&soft.d,level,layer,data.data()+layer*row*rows,row,storage.data()))ot2_host_die("BC fixture swizzle");
        }
        native_bc=false;upload_packaged(soft);tex_t hard=soft;hard.serial=nvn_new_serial();
        if(hardware){native_bc=true;upload_packaged(hard);}native_bc=false;
        auto compare=[&](unsigned level){
            auto a=bc_sample(soft,level);
            if(hardware){auto b=bc_sample(hard,level);for(size_t i=0;i<a.size();i++){
                float tolerance=(f==NVN_FORMAT_BPTC_SFLOAT||f==NVN_FORMAT_BPTC_UFLOAT)?std::max(.005f,std::abs(b[i])*.005f):.015f;
                if(!std::isfinite(a[i])||!std::isfinite(b[i])||std::abs(a[i]-b[i])>tolerance)ot2_host_die("BC native comparison format=%d volume=%d level=%u value=%zu software=%g hardware=%g",f,volume,level,i,a[i],b[i]);
            }}else{
                // Independent endpoint check is covered on the host; here
                // verify the decoded bytes are the ones actually sampled.
                auto& img=image_for(soft);unsigned w=std::max(1U,soft.d.width>>level),h=std::max(1U,soft.d.height>>level),z=volume?std::max(1U,soft.d.depth>>level):3;
                std::vector<uint8_t> decoded(size_t(w)*h*z*info.pixel_bytes),linear(expected[level].size());
                img.bc->read({level,0,0,0,0,w,h,z},linear.data());size_t row=((w+3)/4)*info.block_bytes,rows=(h+3)/4,slice=size_t(w)*h*info.pixel_bytes;
                for(unsigned k=0;k<z;k++)bc_decode(f,{linear.data()+k*row*rows,row*rows},row,w,h,{decoded.data()+k*slice,slice},w*info.pixel_bytes);
                auto raw=make_buffer(nullptr,decoded.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkBufferImageCopy copy{};copy.bufferOffset=raw.offset;copy.imageSubresource={img.aspects,level,0,volume?1U:z};copy.imageExtent={w,h,volume?z:1};
                auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cmd,img.image,img.layout,raw.buffer,1,&copy);finish_commands(cmd);flush_commands();
                if(memcmp(raw.mapped,decoded.data(),decoded.size()))ot2_host_die("BC decoded GPU upload differs");destroy_buffer(raw);
                for(float value:a)if(!std::isfinite(value))ot2_host_die("BC GPU sample is not finite");
            }
        };
        for(unsigned level=0;level<3;level++)compare(level);
        // Partial edge update at the second layer/slice, with padded byte rows.
        std::vector<uint8_t> patch=expected[0];p_copy_b2t update{};update.tex=&soft;update.r={4,4,1,3,1,1};update.src=reinterpret_cast<uint64_t>(patch.data());copy_row_stride=info.block_bytes+3;copy_image_stride=info.block_bytes+3;copy_to_texture(update);
        if(hardware){update.tex=&hard;copy_to_texture(update);}copy_row_stride=copy_image_stride=0;compare(0);
        // A BC->BC copy updates compressed readback as well as sampled pixels.
        tex_t target=soft;target.serial=nvn_new_serial();image_for(target);
        p_copy_t2t cp{};cp.src=&soft;cp.dst=&target;cp.sr=cp.dr={0,0,0,7,5,3};copy_texture(cp);
        auto before=bc_sample(soft,0),after=bc_sample(target,0);if(before!=after)ot2_host_die("BC copy sampled pixels differ");
        std::vector<uint8_t> left(expected[0].size()+19,0xcc),right=left;
        p_copy_b2t read{};read.tex=&soft;read.r=cp.sr;read.src=reinterpret_cast<uint64_t>(left.data());copy_from_texture(read,0,0);read.tex=&target;read.src=reinterpret_cast<uint64_t>(right.data());copy_from_texture(read,0,0);
        if(left!=right||left.back()!=0xcc)ot2_host_die("BC encoded readback/copy guards");
        std::weak_ptr<BcTexture> lease=images.at(soft.serial)->bc;destroy_image(soft.serial);destroy_image(target.serial);if(hardware)destroy_image(hard.serial);
        if(!lease.expired())ot2_host_die("BC encoded storage survives image release");++cases;
    }
    native_bc=hardware;batching=old_batching;copy_row_stride=old_row;copy_image_stride=old_layer;
    if(images.size()!=images_before||image_views.size()!=views_before)ot2_host_die("BC GPU objects leaked");
    printf("PASS BC GPU cases=%u native_comparison=%d all formats/mips/arrays/volumes/partial edges/copy/readback/release\n",cases,hardware);return 1;
}
