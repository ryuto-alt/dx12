// ============================================================================
// ShaderCache.h — グラフ材質のシェーダー（DXIL）のディスクキャッシュとキー（純ロジック。GPU / DXC 非依存）
//
//   キー = hash(生成 HLSL の本文 + include 群の内容ハッシュ + DXC のバージョン + エンジンのバージョン + 追加要素)。
//   ・生成 HLSL は値スロット化されているので、同じ構造のグラフ（コピー / インスタンス / 再オープン）は同じキーになる。
//   ・include 群 = ForwardGraph.hlsl から辿れる全 .hlsl / .hlsli の内容（1 つでも変わればキーが変わる）。
//   ・置き場所: %LOCALAPPDATA%\UnoEngine\shadercache\<エンジン版>\ （環境変数 UNO_SHADERCACHE_DIR で差し替え可）。
//     ファイル名 <キー 16 桁 hex>.<種別>.dxil。ヘッダ（マジック / 形式版 / キー / サイズ / 本体の FNV）で壊れを検出して捨てる。
//   ・書き込みは一時ファイル + rename（別プロセスのエンジンが同じ場所を使っても壊れない）。
//   ・容量の上限管理はしない（DXIL は 1 本 50〜300 KB。エンジン版が変わると別フォルダ）。
//   仕様: docs/MATGRAPH_G2B.md
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

uint64_t Fnv1a64(const void* data, size_t n, uint64_t seed = 1469598103934665603ull);
inline uint64_t Fnv1a64(const std::string& s, uint64_t seed = 1469598103934665603ull) { return Fnv1a64(s.data(), s.size(), seed); }
std::string Hex16(uint64_t v);

// ---- include 群のハッシュ --------------------------------------------------------------
struct IncludeScan
{
    uint64_t                 hash = 0;          // 辿ったファイルの（パス + 内容）の合成
    std::vector<std::string> files;             // 辿れたファイル（絶対パス。辿った順）
    std::vector<std::string> missing;           // 見つからなかった include（診断用）
};

// sourceText の #include "..." を再帰的に辿る（sourceName は先頭ソースのパス。空なら含めない）。
// 解決: 「include 元のフォルダ」→ searchDirs の先頭から。DXC の -I と同じ順序で渡すこと。
IncludeScan ScanIncludes(const std::string& sourceText, const std::vector<std::string>& searchDirs);

// ---- キー ---------------------------------------------------------------------------
struct ShaderKeyInput
{
    std::string hlsl;                // 生成 HLSL の全文（PS 用。VS は空でよい）
    uint64_t    includeHash = 0;     // ScanIncludes().hash
    std::string dxcVersion;          // DXC のバージョン文字列
    std::string engineVersion;       // kEngineVersion
    std::string kind;                // "vs" / "ps"
    std::string extra;               // プロファイル・-D・オプション（"ps_6_6|-HV 2021" など）
};
uint64_t MakeShaderKey(const ShaderKeyInput& in);

// ---- ディスクキャッシュ ----------------------------------------------------------------
class ShaderDiskCache
{
public:
    // dir が空なら DefaultDir(engineVersion)
    explicit ShaderDiskCache(std::string dir = {}, const std::string& engineVersion = {});

    static std::string DefaultDir(const std::string& engineVersion);
    const std::string& Dir() const { return m_dir; }

    // 見つかって壊れていなければ true。kind = "vs" / "ps"
    bool Load(uint64_t key, const std::string& kind, std::vector<uint8_t>& out);
    bool Store(uint64_t key, const std::string& kind, const std::vector<uint8_t>& bytes);

    std::string PathFor(uint64_t key, const std::string& kind) const;

    // 統計（ヒットカウンタ。テスト / MCP の compile 状態が読む）
    std::atomic<uint32_t> hits{0}, misses{0}, stores{0}, corrupt{0};

private:
    std::string m_dir;
};

} // namespace dx12e::matgraph
