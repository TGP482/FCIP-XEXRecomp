// The game makes most of its shaders from game:\shaders.dat only when a draw first needs one (825015F0), so every
// shader in it is compiled ahead on spare cores, and loading screens hold until they are done
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/shaders.h"

namespace gpu {

// a variant's HLSL and its profile, as draws make it
std::string VariantHlsl(ShaderStage stage, const uint32_t* code, size_t dwords, const ShaderOptions& opt,
                        ShaderInfo& info, const wchar_t*& profile);

// the recorded pipelines, made ahead from the archive's shaders
size_t ListedPipelineCount();
void BuildListedPipeline(size_t i);

}  // namespace gpu

namespace d3d {

// once the renderer is up
void PrecompileShaderArchive();
// on a loading screen: true while the archive still compiles, its threads then taking every core
bool ShaderArchiveBusy();
// how much of the archive is done while a loading screen holds for it, else -1
int ShaderArchivePercent();
// the archive's code whose hash is Shader::hash, else null
const std::vector<uint32_t>* ArchiveCode(uint64_t hash);

}  // namespace d3d
