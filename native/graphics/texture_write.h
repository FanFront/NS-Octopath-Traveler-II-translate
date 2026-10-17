#pragma once
#include "nvn_obj.h"
#include "nvn_deswizzle.h"
#include "copy_layout.h"
#include <algorithm>
#include <cstring>
#include <span>

namespace ot2 {
// Copy a host-linear region into NVN backing storage without touching the
// other texels, mip levels, array layers, or row padding.
inline bool write_texture_memory(const tex_t& t,unsigned level,unsigned layer,const NVNcopyRegion& r,
                                 std::span<const uint8_t> source,int64_t row,int64_t slice){
    if(!t.mem||t.d.samples>1||level>=std::max(1u,t.d.levels)||level>=32||
       r.xoffset<0||r.yoffset<0||r.zoffset<0)return false;
    int bw,bh,bytes=nvn_fmt_bpb(t.d.format,&bw,&bh);CopyLayout layout;
    if(!copy_layout(r.width,r.height,r.depth,bw,bh,bytes,row,slice,layout)||source.size()<layout.extent)return false;
    const bool volume=t.d.target==NVN_TEXTURE_TARGET_3D;
    const unsigned w=std::max(1u,t.d.width>>level),h=t.d.target==NVN_TEXTURE_TARGET_1D||t.d.target==NVN_TEXTURE_TARGET_1D_ARRAY?1:std::max(1u,t.d.height>>level);
    const unsigned depth=volume?std::max(1u,t.d.depth>>level):t.L.layers;
    if(unsigned(r.xoffset)>=w||unsigned(r.width)>w-r.xoffset||unsigned(r.yoffset)>=h||unsigned(r.height)>h-r.yoffset||
       layer>=t.L.layers||(volume&&layer)||unsigned(r.zoffset)>=depth||unsigned(r.depth)>depth-r.zoffset||
       (!volume&&(unsigned(r.zoffset)>depth-layer||unsigned(r.depth)>depth-layer-r.zoffset))||
       r.xoffset%bw||r.yoffset%bh||(r.width%bw&&unsigned(r.xoffset+r.width)!=w)||(r.height%bh&&unsigned(r.yoffset+r.height)!=h))return false;
    if(t.d.flags&0x10){
        if(level||layer||r.zoffset||r.depth!=1)return false;
        const size_t pitch=t.d.stride?t.d.stride:(((size_t(w)+bw-1)/bw*bytes+t.L.align-1)&~size_t(t.L.align-1));
        const size_t start=size_t(r.yoffset/bh)*pitch+size_t(r.xoffset/bw)*bytes;
        if(start>t.L.size||layout.rows==0||(layout.rows-1)*pitch+layout.row_bytes>t.L.size-start)return false;
        for(size_t y=0;y<layout.rows;y++)memcpy(t.mem+start+y*pitch,source.data()+y*layout.row_stride,layout.row_bytes);
        return true;
    }
    // Validate every destination subresource before publishing any bytes.
    for(int z=0;z<r.depth;z++){
        uint32_t rb,rows,slices,bpb;uint64_t base;unsigned th,td;
        if(nvn_deswizzle_geom(&t.d,level,volume?0:layer+r.zoffset+z,&rb,&rows,&slices,&base,&th,&td,&bpb))return false;
        for(size_t y=0;y<layout.rows;y++)for(size_t x=0;x<layout.row_bytes;){
            const uint32_t dx=uint32_t(r.xoffset/bw*bytes+x),dy=uint32_t(r.yoffset/bh+y);
            const size_t n=std::min(layout.row_bytes-x,size_t(16-dx%16));
            const uint64_t offset=base+nvn_bl_offset(dx,dy,volume?r.zoffset+z:0,rb,rows,th,td);
            if(offset>t.L.size||n>t.L.size-offset)return false;x+=n;
        }
    }
    for(int z=0;z<r.depth;z++){
        uint32_t rb,rows,slices,bpb;uint64_t base;unsigned th,td;
        nvn_deswizzle_geom(&t.d,level,volume?0:layer+r.zoffset+z,&rb,&rows,&slices,&base,&th,&td,&bpb);
        for(size_t y=0;y<layout.rows;y++)for(size_t x=0;x<layout.row_bytes;){
            const uint32_t dx=uint32_t(r.xoffset/bw*bytes+x),dy=uint32_t(r.yoffset/bh+y);
            const size_t n=std::min(layout.row_bytes-x,size_t(16-dx%16));
            const uint64_t offset=base+nvn_bl_offset(dx,dy,volume?r.zoffset+z:0,rb,rows,th,td);
            memcpy(t.mem+offset,source.data()+size_t(z)*layout.image_stride+y*layout.row_stride+x,n);x+=n;
        }
    }
    return true;
}
}
