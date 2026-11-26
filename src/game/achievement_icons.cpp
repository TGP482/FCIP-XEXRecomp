// A menu sprite loads its texture through the resource loader (8216C440, 823C5200), which only reads the packs, found
// by the CRC of the path in every open pack (823F2018); the textures are plain DDS files. The SPA's PNGs are written
// as DDS files into a pack of our own, laid over game:\Magma\Textures\Achievements and opened with its streamer as
// the game opens common.dat (823EED10). The loader reads through one of its four packs' streamers, so ours stands in
// for the last one while a read of its own is queued.
#include "game/achievement_icons.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "common/custom_levels.h"
#include "core/log.h"
#include "cpu/dispatch.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xam/xam.h"

namespace fcip::achievement_icons {

namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr const char* kDiscFolder = "Magma\\Textures\\Achievements";
// the .fat beside it; the streamer opens the path as it is, so it names the device
constexpr const char* kPackPath = "game:\\Magma\\Textures\\Achievements\\achievements.dat";
constexpr const char* kMenuFolder = "\\Textures\\Achievements\\";  // the sprite loader puts Magma before it

// the packs: up to 32 open, the one last found in, and whether lookups use them
constexpr uint32_t kManagerLast = 0x80u;
constexpr uint32_t kManagerPacks = 0x88u;  // u8
constexpr uint32_t kStreamBlock = 0x10000u;
constexpr uint32_t kStreamBlocks = 4;
// the loader's packs and their streamers, weaponpack, level, common, farcryed; a read names its pack at +0x1C
constexpr uint32_t kLoaderPacks = 0x68u;
constexpr uint32_t kLastLoaderPack = kLoaderPacks + 3 * 8;
constexpr uint32_t kRequestPack = 0x1Cu;
constexpr uint32_t kPathBytes = 0x80u;

// a texture by path in a UI's sprites (a menu's or the HUD's +0x14), as the map list adds them (821684E0)
constexpr uint32_t kStringBytes = 0x1Cu;
constexpr uint16_t kIconTexels[4] = {0, 64, 0, 64};  // left, right, top, bottom of the SPA's 64x64 pictures
constexpr uint32_t kSpriteText = 0x40u;
constexpr uint32_t kSpriteBlockBytes = kStringBytes * 2 + 8 + kSpriteText * 2;  // two std::strings, the texels, their texts
// a pack's FAT: "DAT2", the count, then offset, stored size, size and CRC each, by CRC, little endian
constexpr uint32_t kPackMagic = 0x32544144;

// DDS: magic, a 124 byte header, then A8R8G8B8 rows
constexpr uint32_t kDdsMagic = 0x20534444;
constexpr uint32_t kHeaderBytes = 124;
constexpr uint32_t kHeaderFlags = 0x100F;  // caps, height, width, pitch, pixel format
constexpr uint32_t kFormatBytes = 32;
constexpr uint32_t kFormatFlags = 0x41;  // RGB with alpha
constexpr uint32_t kCapsTexture = 0x1000;

struct Entry {
    uint32_t offset;
    uint32_t size;
    uint32_t crc;
};

int g_state = 0;  // 0 not tried, 1 open, -1 failed
uint32_t g_pack = 0;
uint32_t g_streamer = 0;

void PutText(uint32_t addr, const std::string& text) {
    uint32_t n = std::min<uint32_t>(uint32_t(text.size()), kSpriteText - 1);
    std::memcpy(kernel::HostPtr(addr), text.data(), n);
    kernel::W8(addr + n, 0);
}

fs::path Folder() { return fs::path(kernel::DataDir()) / "achievements"; }

std::string Name(uint32_t image) {
    char name[16];
    std::snprintf(name, sizeof(name), "%u.xbt", image);
    return name;
}

// BGRA rows of a PNG
bool Decode(const std::vector<uint8_t>& png, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    static const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (png.empty()) return false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), DWORD(png.size()));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (FAILED(hr)) return false;
    bgra.resize(size_t(w) * h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()));
}

void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (i * 8)));
}

std::vector<uint8_t> Dds(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    uint32_t header[1 + kHeaderBytes / 4] = {};
    header[0] = kDdsMagic;
    header[1] = kHeaderBytes;
    header[2] = kHeaderFlags;
    header[3] = h;
    header[4] = w;
    header[5] = w * 4;
    uint32_t* format = header + 1 + 18;  // after the 72 byte head of the header
    format[0] = kFormatBytes;
    format[1] = kFormatFlags;
    format[3] = 32;
    format[4] = 0x00FF0000;
    format[5] = 0x0000FF00;
    format[6] = 0x000000FF;
    format[7] = 0xFF000000;
    header[1 + 26] = kCapsTexture;
    std::vector<uint8_t> out;
    for (uint32_t v : header) PutLe32(out, v);
    out.insert(out.end(), bgra.begin(), bgra.end());
    return out;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// every picture the SPA's achievements use, as a pack's data and FAT; the data's size, 0 when nothing was written
uint32_t WritePack() {
    std::set<uint32_t> images;
    for (const xam::Achievement& a : xam::Achievements()) images.insert(a.image);
    std::vector<uint8_t> data;
    std::vector<Entry> entries;
    for (uint32_t image : images) {
        std::vector<uint8_t> bgra;
        UINT w = 0;
        UINT h = 0;
        if (!Decode(xam::AchievementImage(image), bgra, w, h)) continue;
        std::vector<uint8_t> dds = Dds(bgra, w, h);
        std::string key = Lower("\\" + std::string(kDiscFolder) + "\\" + Name(image));
        entries.push_back({uint32_t(data.size()), uint32_t(dds.size()), Crc32(key)});
        data.insert(data.end(), dds.begin(), dds.end());
    }
    if (entries.empty()) return 0;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.crc < b.crc; });
    std::vector<uint8_t> fat;
    PutLe32(fat, kPackMagic);
    PutLe32(fat, uint32_t(entries.size()));
    for (const Entry& e : entries) {
        for (uint32_t v : {e.offset, e.size, e.size, e.crc}) PutLe32(fat, v);
    }
    std::error_code ec;
    fs::create_directories(Folder(), ec);
    std::ofstream dat_file(Folder() / "achievements.dat", std::ios::binary);
    dat_file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    std::ofstream fat_file(Folder() / "achievements.fat", std::ios::binary);
    fat_file.write(reinterpret_cast<const char*>(fat.data()), std::streamsize(fat.size()));
    return dat_file && fat_file ? uint32_t(data.size()) : 0;
}

// opening a pack makes it the one last found in, which the level's own lookups go to first
bool OpenPack(PPCContext& ctx, uint32_t bytes) {
    uint32_t manager = kernel::R32(SYM_PackManager_pointer);
    uint32_t path = mem::AllocSystem(kPathBytes);
    if (!manager || !path) return false;
    std::memcpy(kernel::HostPtr(path), kPackPath, std::strlen(kPackPath) + 1);
    uint32_t last = kernel::R32(manager + kManagerLast);
    uint8_t packs = kernel::R8(manager + kManagerPacks);
    g_pack = uint32_t(kernel::CallGuest(ctx, SYM_PackManager_Open, {manager, path, 0, 0, 0, 0}));
    kernel::W32(manager + kManagerLast, last);
    kernel::W8(manager + kManagerPacks, packs);
    if (g_pack) {
        uint32_t streamers = kernel::R32(SYM_Streamers_pointer);
        uint64_t streamer =
            kernel::CallGuest(ctx, SYM_Streamers_New, {streamers, path, kStreamBlock, kStreamBlocks, 0, bytes});
        g_streamer = uint32_t(streamer);
    }
    mem::FreeSystem(path);
    return g_pack && g_streamer;
}

}  // namespace

bool Ensure(PPCContext& ctx) {
    if (g_state) return g_state > 0;
    kernel::MountDiscOverlay(kDiscFolder, Folder().string());
    uint32_t bytes = WritePack();
    bool ok = bytes && OpenPack(ctx, bytes);
    g_state = ok ? 1 : -1;
    if (!ok) LOGE("achievements: the pictures' pack could not be made");
    return ok;
}

std::string Path(uint32_t image) { return kMenuFolder + Name(image); }

uint32_t Sprite(PPCContext& ctx, uint32_t sprites, uint32_t image) {
    static const uint32_t block = mem::AllocSystem(kSpriteBlockBytes);
    if (!block || sprites < 0x10000u || sprites >= 0xC0000000u || !Ensure(ctx)) return 0;
    std::string name = "achievement_" + std::to_string(image);
    if (uint32_t sprite = uint32_t(kernel::CallGuest(ctx, SYM_Sprites_Find, {sprites, Crc32(name)}))) return sprite;
    uint32_t path_string = block;
    uint32_t name_string = path_string + kStringBytes;
    uint32_t texels = name_string + kStringBytes;
    uint32_t path_text = texels + 8;
    uint32_t name_text = path_text + kSpriteText;
    for (uint32_t i = 0; i < 4; i++) kernel::W16(texels + i * 2, kIconTexels[i]);
    PutText(path_text, Path(image));
    PutText(name_text, name);
    kernel::CallGuest(ctx, SYM_String_Init, {path_string, path_text});
    kernel::CallGuest(ctx, SYM_String_Init, {name_string, name_text});
    kernel::CallGuest(ctx, SYM_Sprites_Add, {sprites, path_string, name_string, texels});
    kernel::CallGuest(ctx, SYM_String_Free, {path_string, 1, 0});
    kernel::CallGuest(ctx, SYM_String_Free, {name_string, 1, 0});
    return uint32_t(kernel::CallGuest(ctx, SYM_Sprites_Find, {sprites, Crc32(name)}));
}

}  // namespace fcip::achievement_icons

PPC_EXTERN_FUNC(__imp__sub_823EDCC8);

// the loader's read (loader, request)
PPC_FUNC(sub_823EDCC8) {
    using namespace fcip::achievement_icons;
    uint32_t slot = uint32_t(ctx.r[3]) + kLastLoaderPack;
    bool ours = g_pack && kernel::R32(uint32_t(ctx.r[4]) + kRequestPack) == g_pack;
    if (!ours) {
        __imp__sub_823EDCC8(ctx, base);
        return;
    }
    uint32_t pack = kernel::R32(slot);
    uint32_t streamer = kernel::R32(slot + 4);
    kernel::W32(slot, g_pack);
    kernel::W32(slot + 4, g_streamer);
    __imp__sub_823EDCC8(ctx, base);
    kernel::W32(slot, pack);
    kernel::W32(slot + 4, streamer);
}
