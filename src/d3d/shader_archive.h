// The game makes most of its shaders from game:\shaders.dat only when a draw first needs one (825015F0), so every
// shader in it is compiled ahead on spare cores, and loading screens hold until they are done
#pragma once

namespace d3d {

// once the renderer is up
void PrecompileShaderArchive();
// on a loading screen: true while the archive still compiles, its threads then taking every core
bool ShaderArchiveBusy();

}  // namespace d3d
