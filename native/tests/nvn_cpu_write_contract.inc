static void cpu_write_contract(){
    NVNtextureBuilder tb{};NVNtexture image{};NVNmemoryPool pool{};NVNmemoryPoolBuilder pb{};
    P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D_ARRAY);P(nvnTextureBuilderSetSize3D)(&tb,20,12,3);P(nvnTextureBuilderSetLevels)(&tb,2);
    const auto storage_size=P(nvnTextureBuilderGetStorageSize)(&tb);std::vector<uint8_t> storage(storage_size);
    P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,storage.data(),storage.size());P(nvnMemoryPoolInitialize)(&pool,&pb);
    P(nvnTextureBuilderSetStorage)(&tb,&pool,0);P(nvnTextureInitialize)(&image,&tb);
    std::vector<uint8_t> initial(20*12*3*4,0x31),actual(initial.size());NVNcopyRegion full{0,0,0,20,12,3};
    P(nvnTextureWriteTexels)(&image,nullptr,&full,initial.data());
    P(nvnTextureReadTexels)(&image,nullptr,&full,actual.data());require(actual==initial,"CPU write visible to GPU readback");
    NVNtextureView view{};P(nvnTextureViewSetDefaults)(&view);P(nvnTextureViewSetLayers)(&view,1,1);
    NVNcopyRegion patch{3,2,0,7,5,1};std::array<uint8_t,40*5> input{};input.fill(0xa7);
    P(nvnTextureWriteTexelsStrided)(&image,&view,&patch,input.data(),40,200);
    for(unsigned y=2;y<7;y++)for(unsigned x=3;x<10;x++)for(unsigned c=0;c<4;c++)initial[((12+y)*20+x)*4+c]=0xa7;
    P(nvnTextureReadTexels)(&image,nullptr,&full,actual.data());require(actual==initial,"partial write preserves other texels and layers");
    const auto* t=reinterpret_cast<const tex_t*>(&image);std::vector<uint8_t> memory_layer(20*12*4);
    require(!nvn_deswizzle(&t->d,0,1,storage.data(),memory_layer.data(),80),"CPU backing can be decoded");
    require(std::equal(memory_layer.begin(),memory_layer.end(),initial.begin()+20*12*4),"CPU backing matches GPU pixels");
    P(nvnTextureViewSetLevels)(&view,1,1);NVNcopyRegion mip{0,0,0,10,6,1};std::vector<uint8_t> mip_input(240,0x5b),mip_actual(240);
    P(nvnTextureWriteTexels)(&image,&view,&mip,mip_input.data());P(nvnTextureReadTexels)(&image,&view,&mip,mip_actual.data());require(mip_input==mip_actual,"CPU writes honor view mip");
    P(nvnTextureReadTexels)(&image,nullptr,&full,actual.data());require(actual==initial,"mip update preserves base level");
    P(nvnTextureFinalize)(&image);P(nvnMemoryPoolFinalize)(&pool);
    puts("PASS NVN CPU texture writes: GPU and backing pixels, partial regions, strides, array layers and mip views");
}
