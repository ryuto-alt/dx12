#pragma once

#include <string>
#include <vector>

namespace dx12e
{

// 1つの .cso 出力(エントリポイント+プロファイル+define の組)。
struct ShaderVariant
{
    const wchar_t* csoName;           // 拡張子込みファイル名。例: L"Forward_VS.cso"
    const wchar_t* entry;             // 例: L"VSMain"
    const wchar_t* profile;           // 例: L"vs_6_0" / L"ps_6_0" / L"cs_6_0"
    const wchar_t* define = nullptr;  // 例: L"LDR_OUTPUT=1"。無ければ nullptr
};

// 1つの .hlsl ソースと、そこから生成される全バリアント。
struct ShaderSource
{
    const char* relPath;                    // shaders/ 基準の相対パス(スラッシュ区切り)。例: "forward/Forward.hlsl"
    std::vector<ShaderVariant> variants;
    std::vector<const char*> staticDeps;    // #include される .hlsli の相対パス(CMake DEPENDS の写し)
};

// CMakeLists.txt の DXC カスタムコマンド群(全61 .cso / 34 .hlsl)の写し。
// CMake 側を変更したら必ずこちらも同期すること(実行時ホットリロードのソース・オブ・トゥルース)。
const std::vector<ShaderSource>& AllShaderSources();

// relPath は大小無視・スラッシュ/バックスラッシュどちらでも一致する。
const ShaderSource* FindShaderSourceByRelPath(const std::string& relPath);

// staticDeps に depRelPath を含むソースを全列挙する(.hlsli 変更時の逆引き用)。
std::vector<const ShaderSource*> FindShaderSourcesByStaticDep(const std::string& depRelPath);

} // namespace dx12e
