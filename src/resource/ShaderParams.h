#pragma once

#include "core/Types.h"

#include <cstddef>
#include <string>
#include <vector>

// ===== カスタムシェーダーの「名前付きパラメーター」 =====
//
// カスタムシェーダーへ渡せる値は、契約ごとに決まった cbuffer の一部分だけ:
//
//   メッシュ (MeshRenderer::shaderPath)
//     PerObjectConstants b0 の mvp(64) + model(64) の後ろ = オフセット 128..159 の float 8 個。
//     ルート定数の予算が 61/64 DWORD で埋まっているので増やせない
//     （内訳は src/graphics/RootSignature.cpp の先頭コメント）。
//
//   画面 (CameraComponent::screenShaderPath)
//     ScreenShaderCB b0 の params(float4) = オフセット 32..47 の float 4 個。
//     残りの 4 つの float4 はエンジンが埋める固定枠（解像度・時刻・カメラ・UV 写像）。
//
// 従来この枠は effectValue / shaderParams / params といった固定の割り当てで、Inspector にも
// 汎用のラベルしか出せず、どのスロットが何を意味するかは書いた本人しか分からなかった。
//
// そこで DXIL のリフレクションで b0 の変数宣言を読み、この範囲に置かれた変数の
// **名前・型・オフセット** を取り出す。Inspector はそれを見て
//
//     float  _Glow;          // @range(0,4)   → 「_Glow」という名前のスライダー
//     float3 _TintColor;     // @color        → 「_TintColor」というカラーピッカー
//
// のように、シェーダーを書いた本人の言葉でウィジェットを並べられる。値の置き場は
// 従来と同じ枠なので、ルートシグネチャもシーン JSON の互換性も一切変わらない。
//
// ★2 つの契約のオフセット範囲は重ならない（メッシュ側の 32..47 は必ず mvp の内側で、
//   行列変数の StartOffset は 0 なので拾われない。画面側の cbuffer は 80 バイトしか無く
//   128 以降に変数を置けない）。だから 1 本のリストに混ぜても取り違えは起こらないが、
//   読む側が場合分けせずに済むよう Param::space に「どちらの枠か」を持たせている。
namespace dx12e::shaderparams
{

// メッシュ用の自由枠。★RootSignature.cpp [0] の内訳
//   mvp(16 DWORD) + model(16 DWORD) + 自由枠(8 DWORD) = 40 DWORD
// と必ず一致させること。ここを変えるなら向こうも変える。
inline constexpr u32 kMeshFreeBegin  = 128;
inline constexpr u32 kMeshFreeEnd    = 160;
inline constexpr u32 kMeshFreeFloats = (kMeshFreeEnd - kMeshFreeBegin) / 4;   // 8

// 画面シェーダー用の自由枠。ScreenShaderPass::Constants の params が載っている位置
//   resolution(16) + timeParams(16) = 32 バイト目から float4 ぶん。
inline constexpr u32 kScreenFreeBegin  = 32;
inline constexpr u32 kScreenFreeEnd    = 48;
inline constexpr u32 kScreenFreeFloats = (kScreenFreeEnd - kScreenFreeBegin) / 4;   // 4

// どちらの契約の枠か。書き込み先のコンポーネントが変わる。
enum class Space : u8
{
    MeshObject,   // MeshRenderer::CustomParamBase() の float[8]
    Screen,       // CameraComponent::screenShaderParams の float[4]
};

// 対応するウィジェット。値の実体は必ず float なので float 系のみ扱う
// （int/bool は同じ 4 バイトでもビット列の意味が違い、JSON 往復で壊れるので非対応）。
enum class Kind : u8
{
    Float,
    Float2,
    Float3,
    Float4,
    Color3,       // float3 + 名前が色っぽい or @color
    Color4,       // float4 + 同上
    Unsupported,  // int / bool / matrix / 配列 など。行は出すが編集させない
};

struct Param
{
    std::string name;                 // HLSL 上の変数名。そのまま Inspector のラベルになる
    std::string typeName;             // "float3" 等。Unsupported の説明に使う
    u32         offset   = kMeshFreeBegin;  // cbuffer 先頭からのバイトオフセット
    Space       space    = Space::MeshObject;
    Kind        kind     = Kind::Float;
    bool        hasRange = false;     // @range(min,max) が付いていた
    f32         minV     = 0.0f;
    f32         maxV     = 1.0f;

    // 自由枠先頭からの float 添字。space に応じて
    // MeshRenderer::CustomParamBase() / CameraComponent::screenShaderParams の添字になる。
    u32 Index() const
    {
        const u32 base = (space == Space::Screen) ? kScreenFreeBegin : kMeshFreeBegin;
        return (offset - base) / 4;
    }

    // ウィジェットが触る float の個数。
    u32 ComponentCount() const;

    // トゥイーンなど「1 個の float を動かす」用途で扱えるか（ベクトルは先頭成分だけ動かす）。
    bool IsAnimatable() const { return kind != Kind::Unsupported; }
};

// シェーダーが「どの契約で書かれているか」。★VS の入力シグネチャから機械的に判る。
// 書き手の申告ではなくファイル自体から導くので、リネームしても移動してもズレない。
//   Mesh   … POSITION + NORMAL（Mesh::GetInputLayout の頂点）
//   Sprite … POSITION あり / NORMAL なし（SpriteRenderer の頂点）
//   Screen … 頂点入力そのものが無い（SV_VertexID からフルスクリーン三角形を作る）
// 契約が違うシェーダーは【割り当てても必ず PSO 生成に失敗する】。選ばせる前に弾けるように、
// Inspector のコンボがこれを見て仕分ける。
enum class Contract : u8
{
    Unknown,   // リフレクションが取れない / どれとも判定できない（従来どおり全部見せる）
    Mesh,      // MeshRenderer::shaderPath
    Sprite,    // Sprite2D::shaderPath
    Screen,    // CameraComponent::screenShaderPath
};
const char* ContractLabel(Contract c);

// 1 本のシェーダーについて分かったこと一式。
struct ShaderInfo
{
    std::vector<Param> params;
    Contract           contract = Contract::Unknown;
    // 任意のグループ名。ソースのどこかに `// @group イベント` と書くと拾う。
    // ★用途で分けたいのは書いた人にしか分からない（イベント用 / 常時 / 試作 …）ので、
    //   そこだけは申告制にする。無ければ Inspector はフォルダ名で代用する。
    std::string        group;
};

// vs/ps の DXIL から b0(space0) の cbuffer を読み、メッシュ用/画面用いずれかの自由枠に
// 載っている変数を列挙する（どちらの契約かはコンパイル時点では分からないので両方見る）。
// hlslSourcePath が読めれば、宣言行の行末コメント `// @range(min,max)` `// @color` も反映する
// （配布ビルドには .hlsl が無いことがあるので、読めなくても失敗にはしない）。
// 返り値: リフレクション自体が成立したら true（変数が 0 個でも true）。
bool Reflect(const void* vs, size_t vsSize, const void* ps, size_t psSize,
             const std::wstring& hlslSourcePath, ShaderInfo& out);

// --- 置き場（プロセス内グローバル。ShaderManager が積み、Inspector / Trigger が読む）---
// ShaderManager はデバイスも ImGui も知らず、InspectorPanel は ShaderManager を持っていない
// ので、shaderdiag の Help/Issue ストアと同じ方式で受け渡す。
// キーは shaderdiag::NormalizeKey() と同じ正規化済み relPath。
void               Set(const std::string& key, ShaderInfo info);
void               Clear(const std::string& key);
std::vector<Param> Get(const std::string& key);

// key のシェーダーが宣言している space 側のパラメーターだけを返す。
std::vector<Param> GetIn(const std::string& key, Space space);

// key のシェーダーの契約 / グループ名。未登録なら Unknown / 空文字。
Contract    GetContract(const std::string& key);
std::string GetGroup(const std::string& key);

// key のシェーダーが宣言している name のパラメーターを探す。無ければ false。
bool Find(const std::string& key, const std::string& name, Param& out);

} // namespace dx12e::shaderparams
