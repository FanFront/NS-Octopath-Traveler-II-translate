#pragma once
#include <array>
#include <cstring>
#include <string>
#include <type_traits>

// Preserve the existing field-by-field wire/cache key exactly, including field
// widths, but grow/check the string once. Never serialize structure padding.
template<class... T> void append_key_fields(std::string& key,const T&... fields){
    static_assert((std::is_trivially_copyable_v<T>&&...));
    std::array<char,(sizeof(T)+...)> bytes;
    size_t offset=0;
    auto copy=[&](const auto& value){std::memcpy(bytes.data()+offset,&value,sizeof(value));offset+=sizeof(value);};
    (copy(fields),...);
    key.append(bytes.data(),bytes.size());
}
