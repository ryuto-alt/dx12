#pragma once
// 環境マップ(スカイボックス / IBL)用の変換。GPU 不要の純 CPU 処理(ctest で単体検証できる)。
//   .hdr(Radiance RGBE) / .exr(OpenEXR) の equirect(正距円筒) → キューブマップ(fp16, ミップ付き)。
// 呼び出し側(TextureLoader::LoadCubeFromEquirectMemory)が結果を .dds としてキャッシュする。

#include <DirectXTex.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace dx12e::envconv
{

// fp16 の上限は 65504。太陽(数万〜数百万)が inf になって IBL 全体を壊さないよう、書き込み前にここへ丸める。
constexpr float kMaxHalf = 60000.0f;
constexpr uint32_t kMaxFace = 1024;   // 面サイズの上限(fp16 キューブ 1024^2 x6 + ミップ = 約 67MB)

// 拡張子(小文字・ドット付き)が equirect として読めるもの(.hdr / .exr)か。
bool IsEquirectExtension(const std::string& lowerExt);

// .exr を RGBA32F の ScratchImage へ。対応: スキャンライン・単一パート・NONE/RLE/ZIPS/ZIP/PIZ・HALF/FLOAT。
// 非対応(タイル・deep・PXR24/B44/DWA)は false を返し outError に理由(標準語)を入れる。
bool DecodeExr(const uint8_t* data, size_t size, DirectX::ScratchImage& out, std::string& outError);

// .hdr / .exr(拡張子で判別)を RGBA32F の ScratchImage へ。
bool DecodeEquirect(const uint8_t* data, size_t size, const std::string& lowerExt,
                    DirectX::ScratchImage& out, std::string& outError);

// 面サイズの決め方: 元画像の幅/4 を 2 のべきへ丸め、[256, maxFace] に収める。
uint32_t ChooseFaceSize(uint32_t srcWidth, uint32_t maxFace = kMaxFace);

// equirect(RGBA32F, 2D) → キューブ(R16G16B16A16_FLOAT, 6 面, 全ミップ)。
// 向き: 画像の中央 = +Z、右 = +X、上 = +Y(D3D の左手系)。面順は D3D(+X -X +Y -Y +Z -Z)。
// 非有限値は 0、上限は kMaxHalf に丸める。
bool EquirectToCube(const DirectX::ScratchImage& equirect, uint32_t faceSize,
                    DirectX::ScratchImage& outCube, std::string& outError);

} // namespace dx12e::envconv
