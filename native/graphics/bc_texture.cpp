#include "bc_texture.h"
#include "nvn.h"
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../third_party/bcdec/bcdec.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

BcFormat bc_format(int f){
    switch(f){
    case NVN_FORMAT_RGB_DXT1:case NVN_FORMAT_RGBA_DXT1:return {NVN_FORMAT_RGBA8,8,4};
    case NVN_FORMAT_RGB_DXT1_SRGB:case NVN_FORMAT_RGBA_DXT1_SRGB:return {NVN_FORMAT_RGBA8_SRGB,8,4};
    case NVN_FORMAT_RGBA_DXT3:case NVN_FORMAT_RGBA_DXT5:case NVN_FORMAT_BPTC_UNORM:return {NVN_FORMAT_RGBA8,16,4};
    case NVN_FORMAT_RGBA_DXT3_SRGB:case NVN_FORMAT_RGBA_DXT5_SRGB:case NVN_FORMAT_BPTC_UNORM_SRGB:return {NVN_FORMAT_RGBA8_SRGB,16,4};
    case NVN_FORMAT_RGTC1_UNORM:return {NVN_FORMAT_R8,8,1};
    case NVN_FORMAT_RGTC1_SNORM:return {NVN_FORMAT_R8SN,8,1};
    case NVN_FORMAT_RGTC2_UNORM:return {NVN_FORMAT_RG8,16,2};
    case NVN_FORMAT_RGTC2_SNORM:return {NVN_FORMAT_RG8SN,16,2};
    case NVN_FORMAT_BPTC_SFLOAT:case NVN_FORMAT_BPTC_UFLOAT:return {NVN_FORMAT_RGBA16F,16,8};
    default:return {};
    }
}
static size_t product(size_t a,size_t b){if(b&&a>std::numeric_limits<size_t>::max()/b)throw std::runtime_error("BC extent overflow");return a*b;}
static void block(int f,const uint8_t* source,uint8_t* out){
    // bcdec uses aligned word accesses internally. Neither NVN transfer strides
    // nor partial uploads promise that alignment.
    alignas(8) uint8_t b[16]{};auto info=bc_format(f);memcpy(b,source,info.block_bytes);
    switch(f){
    case NVN_FORMAT_RGB_DXT1:case NVN_FORMAT_RGBA_DXT1:case NVN_FORMAT_RGB_DXT1_SRGB:case NVN_FORMAT_RGBA_DXT1_SRGB:
        bcdec_bc1(b,out,16);if(f==NVN_FORMAT_RGB_DXT1||f==NVN_FORMAT_RGB_DXT1_SRGB)for(unsigned i=0;i<16;i++)out[i*4+3]=255;break;
    case NVN_FORMAT_RGBA_DXT3:case NVN_FORMAT_RGBA_DXT3_SRGB:bcdec_bc2(b,out,16);break;
    case NVN_FORMAT_RGBA_DXT5:case NVN_FORMAT_RGBA_DXT5_SRGB:bcdec_bc3(b,out,16);break;
    case NVN_FORMAT_RGTC1_UNORM:case NVN_FORMAT_RGTC1_SNORM:bcdec_bc4(b,out,4,f==NVN_FORMAT_RGTC1_SNORM);break;
    case NVN_FORMAT_RGTC2_UNORM:case NVN_FORMAT_RGTC2_SNORM:bcdec_bc5(b,out,8,f==NVN_FORMAT_RGTC2_SNORM);break;
    case NVN_FORMAT_BPTC_UNORM:case NVN_FORMAT_BPTC_UNORM_SRGB:bcdec_bc7(b,out,16);break;
    case NVN_FORMAT_BPTC_SFLOAT:case NVN_FORMAT_BPTC_UFLOAT:{
        uint16_t rgb[48];bcdec_bc6h_half(b,rgb,12,f==NVN_FORMAT_BPTC_SFLOAT);
        for(unsigned i=0;i<16;i++){uint16_t pixel[4]={rgb[i*3],rgb[i*3+1],rgb[i*3+2],0x3c00};memcpy(out+i*8,pixel,8);}break;
    }
    default:throw std::runtime_error("Not a BC format");
    }
}
void bc_decode(int f,std::span<const uint8_t> src,size_t pitch,unsigned w,unsigned h,std::span<uint8_t> dst,size_t dpitch){
    auto info=bc_format(f);if(!info.block_bytes||!w||!h)throw std::runtime_error("Invalid BC decode extent/format");
    size_t row=product((size_t(w)+3)/4,info.block_bytes),rows=(size_t(h)+3)/4,drow=product(w,info.pixel_bytes);
    if(pitch<row||dpitch<drow||src.size()<product(rows-1,pitch)+row||dst.size()<product(h-1,dpitch)+drow)throw std::runtime_error("BC decode span/stride too small");
    for(unsigned y=0;y<h;y+=4)for(unsigned x=0;x<w;x+=4){
        alignas(8) uint8_t decoded[128];block(f,src.data()+size_t(y/4)*pitch+size_t(x/4)*info.block_bytes,decoded);
        unsigned cw=std::min(4U,w-x),ch=std::min(4U,h-y);
        for(unsigned i=0;i<ch;i++)memcpy(dst.data()+size_t(y+i)*dpitch+size_t(x)*info.pixel_bytes,decoded+i*4*info.pixel_bytes,cw*info.pixel_bytes);
    }
}
BcTexture::BcTexture(int f,unsigned w,unsigned h,unsigned d,unsigned levels,unsigned layers,bool volume):
    format_(f),width_(w),height_(h),depth_(d),levels_(levels),layers_(layers),volume_(volume){
    if(!bc_format(f).block_bytes||!w||!h||!d||!levels||levels>32||!layers||(volume&&layers!=1))throw std::runtime_error("Invalid BC texture shape");
}
BcTexture::Shape BcTexture::shape(const BcRegion& r) const{
    if(r.level>=levels_||r.layer>=layers_||!r.width||!r.height||!r.depth)throw std::runtime_error("BC subresource out of bounds");
    unsigned w=std::max(1U,width_>>r.level),h=std::max(1U,height_>>r.level),d=volume_?std::max(1U,depth_>>r.level):1;
    if(r.x>w||r.width>w-r.x||r.y>h||r.height>h-r.y||(r.x%4)||(r.y%4)||((r.width%4)&&r.x+r.width!=w)||((r.height%4)&&r.y+r.height!=h))throw std::runtime_error("BC region must contain complete blocks or image edges");
    if(volume_?(r.z>d||r.depth>d-r.z):(r.z||r.depth>layers_-r.layer))throw std::runtime_error("BC slice range out of bounds");
    size_t row=product((size_t(w)+3)/4,bc_format(format_).block_bytes),rows=(size_t(h)+3)/4;
    return {row,rows,product(row,rows),w,h,d};
}
void BcTexture::transfer(const BcRegion& r,uint8_t* ptr,size_t stride,size_t layer_stride,bool write){
    auto s=shape(r);auto bb=bc_format(format_).block_bytes;
    size_t row=product((size_t(r.width)+3)/4,bb),rows=(size_t(r.height)+3)/4;
    if(!stride)stride=row;if(!layer_stride)layer_stride=product(stride,rows);
    if(!ptr||stride<row||layer_stride<product(stride,rows))throw std::runtime_error("Invalid BC transfer stride");
    product(layer_stride,r.depth); // Check pointer arithmetic before any mutation.
    for(unsigned z=0;z<r.depth;z++){
        auto key=uint64_t(volume_?0:r.layer+z)*levels_+r.level;auto found=encoded_.find(key);
        if(write&&found==encoded_.end())found=encoded_.emplace(key,std::vector<uint8_t>(product(s.slice,s.depth))).first;
        size_t offset=product(volume_?r.z+z:0,s.slice)+size_t(r.y/4)*s.row+size_t(r.x/4)*bb;
        for(size_t y=0;y<rows;y++){
            auto* external=ptr+size_t(z)*layer_stride+y*stride;
            if(write)memcpy(found->second.data()+offset+y*s.row,external,row);
            else if(found!=encoded_.end())memcpy(external,found->second.data()+offset+y*s.row,row);
            else memset(external,0,row); // Uninitialized guest storage has undefined contents.
        }
    }
}
void BcTexture::write(const BcRegion& r,const uint8_t* p,size_t row,size_t slice){transfer(r,const_cast<uint8_t*>(p),row,slice,true);}
void BcTexture::read(const BcRegion& r,uint8_t* p,size_t row,size_t slice){transfer(r,p,row,slice,false);}
size_t BcTexture::bytes() const{size_t total=0;for(auto& [key,data]:encoded_)total+=data.size();return total;}
