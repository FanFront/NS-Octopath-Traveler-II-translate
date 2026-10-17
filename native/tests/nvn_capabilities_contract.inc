// Exercise the actual NVN query entry. These are implementation limits, not
// guesses based on which optional features the underlying GPU advertises.
#include "../graphics/device_limits.h"
#ifndef OT2_FRONTEND_ONLY
#include <vulkan/vulkan.h>
extern "C" VkPhysicalDevice vkhost_test_physical_device();
#endif
static void synthetic_limits_contract(){
    nvn_host_limits small{};
    small.uniform_range=65536;small.uniform_bindings=12;small.storage_bindings=4;small.sampled_images=32;small.samplers=16;small.storage_images=4;
    small.color_targets=4;small.vertex_bindings=8;small.vertex_attributes=12;small.texture_size=4096;small.texture_3d=256;small.texture_cube=2048;small.texture_layers=256;small.buffer_texels=65536;
    small.compute_size[0]=256;small.compute_size[1]=128;small.compute_size[2]=32;small.compute_threads=256;
    small.compute_count[0]=32768;small.compute_count[1]=4096;small.compute_count[2]=1024;small.anisotropy=1;small.subpixel_bits=4;
    struct Expected {NVNdeviceInfo property;int guest,answer;};
    const Expected cases[]={
        {NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE,14,8},{NVN_DEVICE_INFO_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE,16,4},
        {NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE,32,16},{NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE,128,16},
        {NVN_DEVICE_INFO_IMAGE_BINDINGS_PER_STAGE,8,4},{NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS,8,4},
        {NVN_DEVICE_INFO_VERTEX_ATTRIBUTES,16,12},{NVN_DEVICE_INFO_VERTEX_BUFFER_BINDINGS,16,8},
        {NVN_DEVICE_INFO_MAX_TEXTURE_SIZE,16384,4096},{NVN_DEVICE_INFO_MAX_TEXTURE_LEVELS,15,13},
        {NVN_DEVICE_INFO_MAX_3D_TEXTURE_SIZE,2048,256},{NVN_DEVICE_INFO_MAX_CUBE_MAP_TEXTURE_SIZE,16384,2048},
        {NVN_DEVICE_INFO_MAX_TEXTURE_LAYERS,2048,256},{NVN_DEVICE_INFO_MAX_BUFFER_TEXTURE_SIZE,134217728,65536},
        {NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_X,1536,256},{NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Y,1024,128},
        {NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Z,64,32},{NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS,1536,256},
        {NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X,65535,32768},{NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y,65535,4096},
        {NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z,65535,1024},{NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS,1,0},
        {NVN_DEVICE_INFO_MAX_TEXTURE_ANISOTROPY,16,1},{NVN_DEVICE_INFO_SUBPIXEL_BITS,8,4},
        {NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE,65536,65536},{NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE,32,32},
        {NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT,256,256}
    };
    for(auto c:cases)require(nvn_limit_for_host(c.property,c.guest,&small)==c.answer,"synthetic smaller device clamps capabilities without changing guest ABI");
    small.compute_threads=4096;require(nvn_limit_for_host(NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS,1536,&small)==1536,"larger host does not expand the guest compatibility ceiling");
    small.uniform_bindings=3;require(nvn_limit_for_host(NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE,14,&small)==0,"reserved UBO subtraction cannot underflow");
    small.astc=1;require(nvn_limit_for_host(NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS,1,&small)==1,"ASTC follows the enabled native feature");
}
static void capabilities_contract(){
    synthetic_limits_contract();
    auto query=[](NVNdeviceInfo property){int value=-99;P(nvnDeviceGetInteger)(&device,property,&value);return value;};
    require(query(NVN_DEVICE_INFO_MAX_VIEWPORTS)==1,"device reports the single viewport implemented by command replay");
    for(auto property:{NVN_DEVICE_INFO_SUPPORTS_MIN_MAX_FILTERING,NVN_DEVICE_INFO_SUPPORTS_STENCIL8_FORMAT,
        NVN_DEVICE_INFO_SUPPORTS_CONSERVATIVE_RASTER,NVN_DEVICE_INFO_SUPPORTS_ZERO_FROM_UNMAPPED_VIRTUAL_POOL_PAGES,
        NVN_DEVICE_INFO_SUPPORTS_PASSTHROUGH_GEOMETRY_SHADERS,NVN_DEVICE_INFO_SUPPORTS_VIEWPORT_SWIZZLE,
        NVN_DEVICE_INFO_SUPPORTS_SPARSE_TILED_PACKAGED_TEXTURES,NVN_DEVICE_INFO_SUPPORTS_ADVANCED_BLEND_MODES,
        NVN_DEVICE_INFO_SUPPORTS_DRAW_TEXTURE,NVN_DEVICE_INFO_SUPPORTS_TARGET_INDEPENDENT_RASTERIZATION,
        NVN_DEVICE_INFO_SUPPORTS_FRAGMENT_COVERAGE_TO_COLOR,NVN_DEVICE_INFO_SUPPORTS_POST_DEPTH_COVERAGE,
        NVN_DEVICE_INFO_SUPPORTS_SAMPLE_LOCATIONS,NVN_DEVICE_INFO_SUPPORTS_FRAGMENT_SHADER_INTERLOCK,
        NVN_DEVICE_INFO_SUPPORTS_DEBUG_LAYER})
        require(query(property)==0,"unimplemented optional NVN behavior must not be advertised");
    require(query(NVN_DEVICE_INFO_TRANSFORM_FEEDBACK_BUFFER_BINDINGS)==0,"no transform-feedback slots without a replay implementation");
    require(query(NVN_DEVICE_INFO_MAX_PATCH_SIZE)==0,"no tessellation patch support without pipeline state implementation");
    require(query(NVN_DEVICE_INFO_MAX_SAMPLE_LOCATION_TABLE_ENTRIES)==0,"unsupported sample locations have no table capacity");
    require(query(NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE)<=128,"per-stage sampler slots are not descriptor-pool capacity");
    require(query(NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE)==32&&query(NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE)==32,"guest descriptor ABI stays fixed");
    require(query(NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE)==65536&&query(NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT)==256,"guest pool and buffer ABI is independent of host upload alignment");
    require(query(NVN_DEVICE_INFO_MAX_PRESENT_INTERVAL)==5,"present interval matches the host pacer");
    require(query(NVN_DEVICE_INFO_SUPPORTS_IMAGES_USING_TEXTURE_HANDLES)==1,"image handles use the implemented shared descriptor namespace");
    require(query(NVN_DEVICE_INFO_EVENTS_SUPPORT_REDUCTION_OPERATIONS)==1,"WRITE/ADD event operations match the implemented signal path");
    require(query(NVNdeviceInfo(-1))==0&&query(NVNdeviceInfo(0x62))==0,"unknown capability queries return zero");
#ifndef OT2_FRONTEND_ONLY
    VkPhysicalDeviceProperties properties{};VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceProperties(vkhost_test_physical_device(),&properties);
    vkGetPhysicalDeviceFeatures(vkhost_test_physical_device(),&features);
    const auto& native=properties.limits;
    auto bounded=[&](NVNdeviceInfo p,uint32_t maximum){auto value=query(p);require(value>=0&&uint32_t(value)<=maximum,"NVN advertised capability cannot exceed independently queried Vulkan limit");};
    bounded(NVN_DEVICE_INFO_MAX_TEXTURE_SIZE,std::min(native.maxImageDimension1D,native.maxImageDimension2D));
    bounded(NVN_DEVICE_INFO_MAX_3D_TEXTURE_SIZE,native.maxImageDimension3D);bounded(NVN_DEVICE_INFO_MAX_CUBE_MAP_TEXTURE_SIZE,native.maxImageDimensionCube);
    bounded(NVN_DEVICE_INFO_MAX_TEXTURE_LAYERS,native.maxImageArrayLayers);bounded(NVN_DEVICE_INFO_MAX_BUFFER_TEXTURE_SIZE,native.maxTexelBufferElements);
    bounded(NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS,native.maxColorAttachments);bounded(NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS,native.maxFragmentOutputAttachments);
    bounded(NVN_DEVICE_INFO_VERTEX_ATTRIBUTES,native.maxVertexInputAttributes);bounded(NVN_DEVICE_INFO_VERTEX_BUFFER_BINDINGS,native.maxVertexInputBindings);
    bounded(NVN_DEVICE_INFO_MAX_UNIFORM_BUFFER_SIZE,native.maxUniformBufferRange);
    bounded(NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE,native.maxPerStageDescriptorUniformBuffers>4?native.maxPerStageDescriptorUniformBuffers-4:0);
    bounded(NVN_DEVICE_INFO_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE,native.maxPerStageDescriptorStorageBuffers);
    bounded(NVN_DEVICE_INFO_IMAGE_BINDINGS_PER_STAGE,native.maxPerStageDescriptorStorageImages);
    for(auto p:{NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE,NVN_DEVICE_INFO_SEPARATE_TEXTURE_BINDINGS_PER_STAGE,NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE})
        bounded(p,std::min(native.maxPerStageDescriptorSampledImages,native.maxPerStageDescriptorSamplers));
    for(int i=0;i<3;i++){bounded(NVNdeviceInfo(NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_X+i),native.maxComputeWorkGroupSize[i]);bounded(NVNdeviceInfo(NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X+i),native.maxComputeWorkGroupCount[i]);}
    bounded(NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS,native.maxComputeWorkGroupInvocations);
    require(query(NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS)==int(features.textureCompressionASTC_LDR),"ASTC capability matches the enabled device feature");
    printf("NVN_CAPABILITIES native_threads=%u reported_threads=%d native_texture=%u reported_texture=%d reported_viewports=%d\n",native.maxComputeWorkGroupInvocations,query(NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS),native.maxImageDimension2D,query(NVN_DEVICE_INFO_MAX_TEXTURE_SIZE),query(NVN_DEVICE_INFO_MAX_VIEWPORTS));
#endif
    puts("PASS NVN capability report: implemented features, guest ABI and host limits");
}
