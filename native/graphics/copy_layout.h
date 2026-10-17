#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ot2 {
struct CopyLayout {
    size_t row_bytes=0, rows=0, row_stride=0, image_stride=0;
    size_t extent=0, tight_bytes=0, pixels=0;
};
inline bool copy_product(size_t a,size_t b,size_t& out) {
    if(b && a>std::numeric_limits<size_t>::max()/b)return false;
    out=a*b;return true;
}
// Buffer/texture transfers share one layout, including block-compressed data.
// extent is the last byte touched + 1, excluding unused trailing padding.
inline bool copy_layout(int width,int height,int depth,int bw,int bh,int bytes,
                        int64_t row_stride,int64_t image_stride,CopyLayout& out) {
    out={};
    if(width<=0||height<=0||depth<=0||bw<=0||bh<=0||bytes<=0||row_stride<0||image_stride<0)return false;
    CopyLayout p;
    size_t blocks=(size_t(width)+size_t(bw)-1)/size_t(bw);
    p.rows=(size_t(height)+size_t(bh)-1)/size_t(bh);
    if(!copy_product(blocks,size_t(bytes),p.row_bytes))return false;
    p.row_stride=row_stride?size_t(row_stride):p.row_bytes;
    size_t full_image=0;
    if(p.row_stride<p.row_bytes||!copy_product(p.row_stride,p.rows,full_image))return false;
    p.image_stride=image_stride?size_t(image_stride):full_image;
    if(p.image_stride<full_image)return false;
    size_t tail=0,layers=0;
    if(!copy_product(p.rows-1,p.row_stride,tail)||!copy_product(size_t(depth)-1,p.image_stride,layers))return false;
    if(tail>SIZE_MAX-p.row_bytes||layers>SIZE_MAX-(tail+p.row_bytes))return false;
    p.extent=layers+tail+p.row_bytes;
    if(!copy_product(p.row_bytes,p.rows,p.tight_bytes)||!copy_product(p.tight_bytes,size_t(depth),p.tight_bytes)||
       !copy_product(size_t(width),size_t(height),p.pixels)||!copy_product(p.pixels,size_t(depth),p.pixels))return false;
    out=p;return true;
}
inline bool copy_address(uintptr_t base,size_t extent) {
    return base && extent && extent<=UINTPTR_MAX-base;
}
}
