#include "../native_abi.h"
#include <cassert>
#include <cstdio>
struct Pair {float x,y;};
static_assert(NativeAbi<void(*)(void*,int)>::value==1);
static_assert(NativeAbi<double(*)(void*,float,double)>::value==2);
static_assert(NativeAbi<void(*)(int,int,int,int,int,int,int,int,int)>::value==0);
static_assert(NativeAbi<void(*)(Pair)>::value==0);
static_assert(NativeAbi<Pair(*)()>::value==0);
static_assert(NativeAbi<void(*)(const char*,...)>::value==0);
static_assert(NativeAbi<void(*)(__int128)>::value==0);
int main(){
    assert(native_register_abi("nvnCommandBufferBindUniformBuffer")==1);
    assert(native_register_abi("nvnCommandBufferSetDepthBounds")==2);
    assert(native_register_abi("nvnQueuePresentTexture")==0);
    assert(native_register_abi("unknown")==0);
    puts("PASS conservative typed native bridge ABI and generic fallbacks");
}
