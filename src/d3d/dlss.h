// NVIDIA DLSS through Streamline, whose DLLs the player puts beside the exe
#pragma once

struct ID3D12Device;

namespace dlss {

// before the device: Streamline from beside the exe, only when NVIDIA signed it
void Load();
// the device made: what this GPU and driver support
void SetDevice(ID3D12Device* dev);
// before the device goes
void Shutdown();

bool SuperResolution();
bool FrameGeneration();

}  // namespace dlss
