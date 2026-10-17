#include "../graphics/bc_texture.h"
#include "../graphics/nvn.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
static void check(bool ok){if(!ok)throw std::runtime_error("BC host contract");}
int main(){
    std::array<uint8_t,18> src{};src[1]=0;src[2]=0;src[3]=0xff;src[4]=0xff;for(int i=5;i<9;i++)src[i]=0xff;
    std::array<uint8_t,100> out;out.fill(0xcc);
    for(auto f:{NVN_FORMAT_RGB_DXT1,NVN_FORMAT_RGBA_DXT1}){
        bc_decode(f,{src.data()+1,8},8,3,2,{out.data()+1,99},17);
        for(unsigned y=0;y<2;y++)for(unsigned x=0;x<3;x++)check(out[1+y*17+x*4+3]==(f==NVN_FORMAT_RGB_DXT1?255:0));
        check(out[0]==0xcc&&out[13]==0xcc&&out[30]==0xcc);
    }
    for(bool volume:{false,true}){
        BcTexture texture(NVN_FORMAT_RGBA_DXT1,7,5,3,3,volume?1:3,volume);
        BcRegion full{0,0,0,0,0,7,5,3};std::vector<uint8_t> in(150,0xcc),read(150,0xcc);
        for(unsigned z=0;z<3;z++)for(unsigned y=0;y<2;y++)for(unsigned x=0;x<16;x++)in[z*50+y*23+x]=uint8_t(z*37+y*16+x);
        texture.write(full,in.data(),23,50);texture.read(full,read.data(),23,50);check(in==read);check(texture.bytes()==96);
        BcRegion tail{0,volume?0U:1U,4,4,volume?1U:0U,3,1,1};uint8_t patch[8]={12,31,25,77,0,98,32,43};texture.write(tail,patch);
        texture.read(full,read.data(),23,50);for(unsigned i=0;i<8;i++)in[50+23+8+i]=patch[i];check(in==read);
        BcRegion mip{2,volume?0U:2U,0,0,0,1,1,1};texture.write(mip,patch);uint8_t tiny[8];texture.read(mip,tiny);check(!memcmp(tiny,patch,8));check(texture.bytes()==104);
        bool rejected=false;try{texture.write({0,0,1,0,0,4,4,1},patch);}catch(const std::runtime_error&){rejected=true;}check(rejected);
    }
    for(int f=NVN_FORMAT_RGB_DXT1;f<=NVN_FORMAT_BPTC_UFLOAT;f++){
        auto info=bc_format(f);check(info.block_bytes&&info.pixel_bytes);
        std::array<uint8_t,16> zero{};std::array<uint8_t,128> pixels{};bc_decode(f,zero,info.block_bytes,4,4,pixels,info.pixel_bytes*4);
    }
    std::cout<<"PASS BC formats, RGB/alpha, unaligned bytes, odd edges, strides, partial blocks, mips, arrays, volumes, bounds and image-owned byte limits\n";
}
