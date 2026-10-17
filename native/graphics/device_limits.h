#pragma once
#include "nvn.h"
#include <stdint.h>

// Vulkan-independent limits so the query policy can also be exercised with
// deliberately smaller synthetic devices. Alignment here is intentionally
// absent: guest buffers are copied into aligned host allocations.
typedef struct {
    uint32_t uniform_range,uniform_bindings,storage_bindings,sampled_images,samplers,storage_images;
    uint32_t color_targets,vertex_bindings,vertex_attributes;
    uint32_t texture_size,texture_3d,texture_cube,texture_layers,buffer_texels;
    uint32_t compute_size[3],compute_count[3],compute_threads;
    uint32_t anisotropy,astc,subpixel_bits;
} nvn_host_limits;

static inline uint32_t nvn_limit_min(uint32_t a,uint32_t b){return a<b?a:b;}
static inline int nvn_limit_for_host(NVNdeviceInfo property,int guest_maximum,const nvn_host_limits* host){
    uint32_t value=(uint32_t)guest_maximum;
    switch(property){
    // cbuf 0/1/2 and the translation support UBO consume at most four
    // additional descriptors in each stage beyond the guest UBO slots.
    case NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE:value=host->uniform_bindings>4?host->uniform_bindings-4:0;break;
    case NVN_DEVICE_INFO_MAX_UNIFORM_BUFFER_SIZE:value=host->uniform_range;break;
    case NVN_DEVICE_INFO_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE:value=host->storage_bindings;break;
    case NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS:value=host->color_targets;break;
    case NVN_DEVICE_INFO_VERTEX_BUFFER_BINDINGS:value=host->vertex_bindings;break;
    case NVN_DEVICE_INFO_VERTEX_ATTRIBUTES:value=host->vertex_attributes;break;
    case NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE:
    case NVN_DEVICE_INFO_SEPARATE_TEXTURE_BINDINGS_PER_STAGE:
    case NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE:value=nvn_limit_min(host->sampled_images,host->samplers);break;
    case NVN_DEVICE_INFO_IMAGE_BINDINGS_PER_STAGE:value=host->storage_images;break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_ANISOTROPY:value=host->anisotropy;break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_X:value=host->compute_size[0];break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Y:value=host->compute_size[1];break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Z:value=host->compute_size[2];break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS:value=host->compute_threads;break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X:value=host->compute_count[0];break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y:value=host->compute_count[1];break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z:value=host->compute_count[2];break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_SIZE:
    case NVN_DEVICE_INFO_MAX_RECTANGLE_TEXTURE_SIZE:value=host->texture_size;break;
    case NVN_DEVICE_INFO_MAX_3D_TEXTURE_SIZE:value=host->texture_3d;break;
    case NVN_DEVICE_INFO_MAX_CUBE_MAP_TEXTURE_SIZE:value=host->texture_cube;break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_LAYERS:value=host->texture_layers;break;
    case NVN_DEVICE_INFO_MAX_BUFFER_TEXTURE_SIZE:value=host->buffer_texels;break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_LEVELS:{value=0;for(uint32_t size=host->texture_size;size;size>>=1)++value;break;}
    case NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS:value=host->astc;break;
    case NVN_DEVICE_INFO_SUBPIXEL_BITS:value=host->subpixel_bits;break;
    default:break;
    }
    return (int)nvn_limit_min((uint32_t)guest_maximum,value);
}
