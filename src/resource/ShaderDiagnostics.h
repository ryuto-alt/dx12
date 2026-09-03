#pragma once

#include "core/Types.h"

#include <cstddef>
#include <string>
#include <vector>

#include <directx/d3d12.h>
#include <d3d12shader.h>
#include <wrl/client.h>

// ===== カスタムシェーダーの「なぜ動かないか」を人が読める形にする =====
//
// カスタムシェーダー（MeshRenderer::shaderPath / Sprite2D::shaderPath /
// CameraComponent::screenShaderPath）が効かない理由は 2 つしかない:
//
//   1. HLSL がコンパイルできない  … DXC が行番号付きのエラーを出すので分かりやすい
//   2. PSO の生成に失敗する        … ★従来はここが "PSO生成に失敗しました" の一行だけだった
//
// 2 の実体はほぼ全て「ルートシグネチャに無いレジスタを宣言した」。D3D12 はその詳細を
// デバッグレイヤー経由でしか吐かず、Release ビルドのユーザーには何も見えない。
// そこで【シェーダーのリフレクション】と【直列化済みルートシグネチャ】を突き合わせ、
// どの register が余計なのかを自力で特定して、使えるスロット一覧＝書式ごと提示する。
//
// この仕組みはデバッグレイヤーに依存しないので、配布ゲーム側でも同じ説明が出る。
namespace dx12e::shaderdiag
{

// シェーダーが実際に宣言していたリソース 1 個ぶん。
struct Binding
{
    char        kind = 't';  // 'b'=cbuffer / 't'=SRV / 'u'=UAV / 's'=sampler
    u32         reg   = 0;    // register 番号
    u32         space = 0;    // register space
    u32         count = 1;    // 配列長（0=無制限）
    std::string name;         // HLSL 上の名前
};

// スロットに付ける日本語の説明。契約ごとに手で書く（無いスロットは番号だけ出る）。
struct SlotNote
{
    char        kind;
    u32         reg;
    const char* jp;
};

// 「このシェーダーは何を守るべきか」の定義。ルートシグネチャの直列化バイト列を
// そのまま持つので、注記を書き忘れても実際に使えるスロットは常に正しく列挙される。
struct Contract
{
    std::string title;      // "スクリーンシェーダー（カメラの画面シェーダー）"
    std::string entryNote;  // "VSMain / PSMain（vs_6_0 / ps_6_0）"
    std::string extra;      // 頂点入力など、レジスタ以外の約束事

    const void* rsBlob     = nullptr;  // 直列化済みルートシグネチャ
    size_t      rsBlobSize = 0;

    const SlotNote* notes     = nullptr;
    size_t          noteCount = 0;
};

// DXIL コンテナから ID3D12ShaderReflection を作る。dxcompiler.dll が無い / リフレクション部が
// 剥がされている等で失敗したら nullptr。
// ★リフレクションの生成手順(IDxcUtils + DxcBuffer)はここに一本化する。ShaderParams など
//   他のモジュールもこれを通すこと(DxcBuffer::Encoding の指定を誤ると黙って失敗する)。
Microsoft::WRL::ComPtr<ID3D12ShaderReflection> CreateReflection(const void* bytecode, size_t size);

// DXIL コンテナからリソース宣言を取り出す。dxcompiler.dll が無い等で失敗したら false。
bool Reflect(const void* bytecode, size_t size, std::vector<Binding>& out);

// 契約を人間向けの「書式」テキストにする（エラーが無くてもヘルプとして出せる）。
std::string DescribeContract(const Contract& c);

// PSO 生成に失敗したときの説明文を組み立てる。
// vs/ps のリフレクションが取れれば「余計な register」を名指しし、取れなければ
// よくある原因のチェックリストへ落とす。どちらの場合も末尾に書式を付ける。
// detail は例外の what() など、呼び出し側が持っている追加情報（空可）。
std::string ExplainPsoFailure(const Contract& c, HRESULT hr,
                              const void* vs, size_t vsSize,
                              const void* ps, size_t psSize,
                              const std::string& detail = {});

// ルートシグネチャに無いレジスタだけを列挙する（PSO を作る前の事前検査用）。
// 返り値が空なら「レジスタ的には問題なし」。
std::vector<Binding> FindUnsupportedBindings(const Contract& c,
                                             const void* vs, size_t vsSize,
                                             const void* ps, size_t psSize);

// --- 書式ヘルプの置き場（エディタ UI から契約 ID で引く）---
// ルートシグネチャを持っている側（ScreenShaderPass / RootSignature / SpriteRenderer）が
// 初期化時に登録する。UI 側はデバイスに触らずにテキストだけ読める。
inline constexpr const char* kIdScreen = "screen";
inline constexpr const char* kIdMesh   = "mesh";
inline constexpr const char* kIdSprite = "sprite";

void        RegisterHelp(const std::string& contractId, std::string text);
std::string GetHelp(const std::string& contractId);

// --- 直近の不具合の置き場（キー = 正規化済みの shaderRel）---
// コンパイル失敗も PSO 失敗もここへ積み、Inspector / 診断 / MCP が同じものを読む。
void        SetIssue(const std::string& key, std::string text);
void        ClearIssue(const std::string& key);
std::string GetIssue(const std::string& key);
// 正規化: 小文字・'/' 区切り・先頭の "shaders/" を剥がす（Application 側と同じ規則）。
std::string NormalizeKey(const std::string& shaderRel);

// text（ログ 1 行の全文など）の中に、問題を抱えているシェーダーのパスが含まれていたら
// その正規化キーを返す。無ければ空。大小は無視する。
// ★コンソールのエラー行から「そのシェーダーの Inspector」へ飛ぶために使う。
//   ログの文面を解析するのではなく、実際に問題として積まれているキーを照合するので、
//   メッセージの書き方を変えても壊れない。
std::string FindIssueKeyIn(const std::string& text);

} // namespace dx12e::shaderdiag
