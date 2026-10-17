#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

// Original NVN format remains the transfer/storage contract. Only the host
// sampled image is expanded. Encoded bytes belong to the live image, not a
// process-wide cache, and are released when that texture is finalized.
struct BcFormat { int unpacked{}; unsigned block_bytes{},pixel_bytes{}; };
BcFormat bc_format(int format);
void bc_decode(int format,std::span<const uint8_t> source,size_t pitch,
               unsigned width,unsigned height,std::span<uint8_t> destination,size_t dst_pitch);
struct BcRegion {unsigned level{},layer{},x{},y{},z{},width{},height{},depth{};};
class BcTexture {
    int format_;
    unsigned width_,height_,depth_,levels_,layers_;
    bool volume_;
    std::map<uint64_t,std::vector<uint8_t>> encoded_;
    struct Shape {size_t row,rows,slice;unsigned width,height,depth;};
    Shape shape(const BcRegion&) const;
    void transfer(const BcRegion&,uint8_t*,size_t,size_t,bool);
public:
    BcTexture(int format,unsigned width,unsigned height,unsigned depth,unsigned levels,unsigned layers,bool volume);
    void write(const BcRegion&,const uint8_t*,size_t row_stride=0,size_t layer_stride=0);
    void read(const BcRegion&,uint8_t*,size_t row_stride=0,size_t layer_stride=0);
    size_t bytes() const;
};
