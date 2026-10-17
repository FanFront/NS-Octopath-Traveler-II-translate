#pragma once
#include <cstdint>
#include <span>
#include <vector>

// Specialize known unbound descriptors, not arbitrary values or draws. Removed
// reads return a typed zero and writes/atomics have no memory side effects.
std::vector<uint32_t> shader_null_descriptors(std::span<const uint32_t> words,
                                            std::span<const uint32_t> bindings);
constexpr uint32_t depth_bounds_uniform_binding=4094,depth_bounds_input_binding=4095;
// Read-only, single-sample depth bounds: test the existing depth attachment,
// not gl_FragCoord.z. The backend rejects depth/stencil writes and side effects.
std::vector<uint32_t> shader_readonly_depth_bounds(std::span<const uint32_t> words);
// The offline translator declares fragment interlock even when no interlock
// operation/mode is present. Remove only that demonstrably unused declaration.
std::vector<uint32_t> shader_without_unused_interlock(std::span<const uint32_t> words);
constexpr uint32_t software_samples_binding=4093;
// Exact surviving single-sample fragment count. The backend permits this only
// without depth/stencil tests, sample masks, alpha-to-coverage, early-fragment
// tests or multisampling.
// Two 32-bit atomics retain a 64-bit count without requiring int64 atomics.
std::vector<uint32_t> shader_count_samples(std::span<const uint32_t> words);
// Maxwell texture registers carry raw 32-bit values. Specialize the Vulkan
// declaration to the actual integer view, retaining those bits at shader uses.
struct ShaderIntegerImage {uint32_t binding, signedness, image_format;};
std::vector<uint32_t> shader_integer_images(std::span<const uint32_t> words,
                                          std::span<const ShaderIntegerImage> images);
struct ShaderIntegerOutput {uint32_t location,signedness;};
std::vector<uint32_t> shader_integer_outputs(std::span<const uint32_t> words,
                                           std::span<const ShaderIntegerOutput> outputs);
