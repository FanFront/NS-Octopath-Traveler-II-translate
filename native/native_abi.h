#pragma once
#include <type_traits>

// Deliberately conservative AAPCS64 subset: at most eight scalar arguments,
// each at most one 64-bit register. No aggregates, vectors, varargs or int128.
// 0 = generic bridge, 1 = integer/pointer inputs, 2 = also FP inputs.
template<class T> constexpr bool native_scalar(){
    if constexpr(std::is_void_v<T>)return true;
    else return std::is_scalar_v<T>&&(sizeof(T)<=8);
}
template<class T> struct NativeAbi {static constexpr unsigned value=0;};
template<class R,class... A> struct NativeAbi<R(*)(A...)> {
    static constexpr unsigned value=(sizeof...(A)<=8&&native_scalar<R>()&&(native_scalar<A>()&&...))?
        ((std::is_floating_point_v<A>||...)?2:1):0;
};
unsigned native_register_abi(const char* name);
