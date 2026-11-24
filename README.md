# Far Cry Instincts: Predator - XEX Recomp

An unofficial native PC port of the Xbox 360 version of Far Cry Instincts: Predator, with extended graphics options, keyboard and mouse support, and other enhancements.

This port was created using the [Xbox 360 XEX Recomp](https://github.com/TGP482/Xbox360XEX-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files.
- Run `FCIP.exe`. The first start compiles the game's code for your PC, this only happens once.
- Recomp settings can be adjusted in the in-game options menu while the game is running, or through the Settings overlay, which is opened with F10 by default.
<img width="1920" height="1080" alt="{093FE4A3-2CAB-4354-94DA-DD0CFA02B7EF}" src="https://github.com/user-attachments/assets/def470af-3424-4f7b-910c-f9d327352faa" />

## Features

### Display
- **Monitor** - Picks the monitor the game shows on, for Fullscreen and Borderless (and where Windowed opens). Lists every connected monitor by its Windows number and desktop resolution.
- **Display Mode** - Switch between Fullscreen, Borderless and Windowed.
- **Resolution** - Lists every mode your monitor reports and defaults to your desktop resolution, replacing the original 1280x720 resolution.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, allowing supersampling or downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 90 being the game's default. Cutscenes keep the original.
- **Viewmodel Field of View** - Adjusts the first-person weapon and arms field of view.
- **V-Sync** - Reduces screen tearing.
- **HDR Output** - Enables HDR10 output on displays with Windows HDR turned on, detected automatically. Highlights extend up to the display's peak brightness.
- **Max Frame Rate** - Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap entirely, replacing the Xbox 360's 30 FPS limiter.
- **FPS Counter** - Displays the frame rate on screen.
- **Colour Blind Mode** - Adjusts the colours for Protanopia, Deuteranopia or Tritanopia.

### Graphics
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO, GTAO and hardware ray traced RTAO.
- **Anti Aliasing** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image, TAA, which smooths every edge by blending each frame with the last, or AMD FSR 3, which also upscales with FSR when the render resolution is lowered.
- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
- **Shadow Resolution** - Sets the sun shadow resolution from 1024x1024 up to 8192x8192.
- **Shadow Filtering** - Choose between Sharp, the game's own shadow edges, Soft, PCSS and Ray Traced.
- **Draw Distance** - Increases how far objects, grass, vegetation and shadows are drawn up to 32x.
- **Aim and Sprint Blur** - Toggles the blur effect while aiming down a scope and during feral run.
- **Global Illumination** - Adds hardware ray traced global illumination, so light bounces off every surface.
- **Volumetric Fog** - Replaces the game's fog with volumetric height fog.
- **Volumetric Clouds** - Adds volumetric to the sky.
- **Caustics** - Adds the rippling sunlight the water casts on everything under the ocean and rivers.
- **Ocean Reflection** - Choose between Xbox 360 and Original Xbox.
- **Frame Generation** - Generates one, two or three frames between each two rendered ones (2X, 3X or 4X) with AMD FSR Frame Generation.

### General
- **Skip Intros** - Skips the Ubisoft logo, the branding plate and the intro movie.
- **Pause On Focus Loss** - Pauses the game and its sound while the window is in the background or minimized.
- **Language** - Choose between Automatic, which follows the Windows display language, English, French, German, Italian and Spanish.
- **Audio Output** - Choose between Automatic, which follows the speakers set up in Windows, Mono, 2.0 Stereo and 5.1 Surround.
- **System Link Support** - System Link can be played online by directly connecting to a friend's IP address, or over LAN.


### Textures
- **Texture Replacement** - Replaces the game's textures with the `.dds` or `.png` images in the `textures\replacement` folder beside the exe.
- **Texture Dumping** - Saves every texture the game uses into the `textures\dumps` folder beside the exe as you play.
- **Dump Format** - Choose between PNG or DDS, which keeps the original compression and mipmaps.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with raw input.
- **Controller** - Controller with SDL3 support.
- **Gyro Aiming** - Adds gyro aiming for controllers with motion sensors, such as the DualSense, DualShock 4 and Switch Pro Controller. Works together with the right stick.
- 
### Miscellaneous
- **Custom Menu Cursor** - Place a `cursor.png` (or `cursor.dds`) beside the exe to replace the Windows arrow in the game's menus.
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.

## Playing Online
System Link can be played online by connecting directly to the host's IP address. No third-party programs are needed.
- Enter the host's IP address in **Online Join IP**, from the Multiplayer menu or the Settings overlay.
- From the System Link menu, join the host's session from the list.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **CPU** with support for SSE4.2 and POPCNT
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Direct3D 12 and Shader Model 6.0
  - NVIDIA GeForce GT 630 (Kepler)
  - AMD Radeon HD 7750
  - Intel HD Graphics 510 (Skylake)
- **Memory**
  - 4 GB
- **Storage**
  - 6 GB for the game files, the compiled game code, saves and shader cache

## Building

### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xex` must be there, with the game's folders beside it.

### 3. Install Dependencies
Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).

### 4. Build the Project
Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to `out\build\release`.

## Credits

- AMD FSR 3, built from the open source [AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) under the MIT license. AMD, FSR and FidelityFX are trademarks of Advanced Micro Devices, Inc.
