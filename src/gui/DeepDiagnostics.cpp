#include "gui/DeepDiagnostics.h"

#include "core/Application.h"
#include "core/PathResolver.h"
#include "core/Version.h"
#include "ecs/Components.h"
#include "renderer/DrawItem.h"
#include "renderer/Mesh.h"
#include "resource/ModelLoader.h"
#include "resource/ShaderManager.h"
#include "resource/ShaderRegistry.h"
#include "resource/TextureLoader.h"
#include "scene/Scene.h"
// ※ terrain/HeightField.h からは *ヘッダ内 inline のアクセサだけ* を使う
//    （IsValid / Resolution / WorldSize）。Gui は Terrain ライブラリをリンクしていないので、
//    .cpp 側にある関数（Create / Decode / NormalizeResolution 等）を呼ぶとリンクエラーになる。
#include "terrain/HeightField.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace dx12e
{
namespace
{
namespace fs = std::filesystem;

// 1 検査あたりの表示上限。3000 枚のテクスチャが全部同じ理由で落ちても
// パネルとクリップボードが埋まらないようにする（超過分は omitted に数だけ残す）。
constexpr int kMaxIssues = 40;

// 走査上限。超詳細診断は遅くて良いが「無限に待たされる」は困る。
// 打ち切ったら必ず skipped に書く（黙って一部だけ見て緑、を作らない）。
constexpr int kMaxTextures = 1500;
constexpr int kMaxModels   = 80;

// SkinningBuffer は maxBones を超えたぶんを無言で切り捨てる（SkinningBuffer.cpp の copyCount）。
// 値の出所は Skeleton::kMaxBones。Animation へリンクを増やさないためここに写している。
constexpr uint32_t kMaxBonesRef = 256;

// 1 フレームに GPU へ送れるライトの数。超えたぶんは *無言で* 描画されない。
// クラスタードライティング（Forward+）化で「点 8 / スポット 8」の個別上限は撤廃され、
// 今は point + spot の合計 1024 灯。ただし 1 クラスタ（画面を 16x9x24 に割った 1 マス）で
// 評価できるのは 128 灯までで、そこを超えた分も無言で消える（CPU からは検出できない）。
// 出所は src/renderer/ClusterMath.h の kMaxSceneLights / kMaxLightsPerCluster
// （= src/editor/LightingPresets.h の kLightBudgetTotal / kLightBudgetPerCluster）。写している。
constexpr int kMaxTotalLightsRef  = 1024;
constexpr int kMaxPerClusterRef   = 128;
// 影スロットの同時上限。出所は Application.h の kMaxShadowSpot / kMaxShadowPoint。
// あぶれたライトは「影だけ」落ちる（ライト自体は出る）＝気付きにくいので注意で出す。
constexpr int kMaxShadowSpotRef  = 4;
constexpr int kMaxShadowPointRef = 2;

// ScenePickOptions::maxCandidates（editor/ScenePick.h）。ナローフェーズに掛ける候補数の歯止め。
constexpr int kPickCandidateLimitRef = 64;

// 1 検査あたりの走査上限（.lua）。
constexpr int kMaxScripts = 400;

// 数値を指摘文へ埋めるときの共通書式（既定 小数 2 桁）。
std::string Fmt(float v, int digits = 2)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, static_cast<double>(v));
    return buf;
}

// ★ 検査関数 DeepDiag::Terrain() が、同名のコンポーネント dx12e::Terrain を
//   *関数の中では* 名前で隠してしまう（unqualified な Terrain は関数を指す）。
//   コンポーネントを指すときは必ずこの別名を使うこと。
using TerrainComp = dx12e::Terrain;

std::string ToUtf8Lower(const fs::path& p)
{
    std::string s = p.string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool HasExt(const fs::path& p, std::initializer_list<const char*> exts)
{
    const std::string e = ToUtf8Lower(p.extension());
    for (const char* want : exts)
        if (e == want) return true;
    return false;
}

// ファイル名(拡張子なし)が法線マップっぽいか。フォルダ名は見ない
// （"enso_normal/" のような画面名フォルダを法線と誤認しないため）。
bool LooksLikeNormalMap(const fs::path& p)
{
    const std::string s = ToUtf8Lower(p.stem());
    return s.find("normal") != std::string::npos || s.find("_nrm") != std::string::npos
        || s.find("_norm") != std::string::npos || (s.size() >= 2 && s.compare(s.size() - 2, 2, "_n") == 0);
}

bool LooksLikeDataMap(const fs::path& p)
{
    const std::string s = ToUtf8Lower(p.stem());
    return s.find("roughness") != std::string::npos || s.find("metal") != std::string::npos
        || s.find("_mr") != std::string::npos || s.find("_orm") != std::string::npos
        || s.find("height") != std::string::npos || s.find("_ao") != std::string::npos;
}

bool IsSrgbFormatName(const std::string& format)
{
    return format.find("SRGB") != std::string::npos;
}

bool Finite3(const float v[3])
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// 診断で扱う範囲のフォーマット名。TextureLoader の表と違い、ここは表示パイプラインの
// 数種類しか通らないので短い表で足りる（未知は数値で出す＝そこも気付けるように）。
std::string FormatName(unsigned f)
{
    switch (f)
    {
    case 28:  return "R8G8B8A8_UNORM";
    case 29:  return "R8G8B8A8_UNORM_SRGB";
    case 10:  return "R16G16B16A16_FLOAT";
    case 2:   return "R32G32B32A32_FLOAT";
    case 40:  return "D32_FLOAT";
    case 87:  return "B8G8R8A8_UNORM";
    case 91:  return "B8G8R8A8_UNORM_SRGB";
    case 24:  return "R10G10B10A2_UNORM";
    default:  break;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "DXGI_FORMAT(%u)", f);
    return buf;
}

// root からの相対パスを表示用文字列にする。例外を投げない版（fs::relative は throw しうる）。
std::string RelDisplay(const fs::path& p, const fs::path& root)
{
    std::error_code ec;
    const fs::path rel = fs::relative(p, root, ec);
    return (ec || rel.empty()) ? p.string() : rel.generic_string();
}

// assets 相対パスを実ファイルへ解決する。空文字列とプリミティブマーカーは対象外。
bool ResolveAssetPath(const std::string& rel, fs::path& out)
{
    if (rel.empty()) return false;
    if (rel.rfind("__primitive", 0) == 0) return false;   // __primitive_box__ 等はファイルではない
    if (rel.rfind("__terrain", 0) == 0)   return false;   // 地形メッシュ(CPU生成)もファイルではない
    if (rel.rfind("__sculpt", 0) == 0)    return false;   // スカルプトメッシュ(.smsh から復元)も同様
    out = fs::path(PathResolver::AssetsDir()) / rel;
    return true;
}

// 走査対象のファイルを集める。assets が無い/読めない場合は空を返す（呼び出し側でエラーにする）。
std::vector<fs::path> CollectFiles(const fs::path& root, std::initializer_list<const char*> exts)
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;

    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        if (HasExt(it->path(), exts)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());   // 打ち切り時に毎回同じ集合を見るように順序を固定
    return out;
}

// .cso の中身が DXIL/DXBC コンテナとして最低限まともか。
// dxc の出力は DXIL でもコンテナのマジックは 'DXBC'。
bool CsoLooksValid(const fs::path& p, uintmax_t& sizeOut)
{
    std::error_code ec;
    sizeOut = fs::file_size(p, ec);
    if (ec || sizeOut < 32) return false;

    std::ifstream f(p, std::ios::binary);
    char magic[4] = {};
    if (!f.read(magic, 4)) return false;
    return std::memcmp(magic, "DXBC", 4) == 0;
}

// 指摘文に出す「誰の話か」。名前が無いエンティティでも一意に指せるようにする。
std::string EntityLabel(const entt::registry& reg, entt::entity e)
{
    if (reg.all_of<NameTag>(e)) return reg.get<NameTag>(e).name;
    return "entity#" + std::to_string(static_cast<std::uint32_t>(e));
}

bool Finite1(float v) { return std::isfinite(v); }

bool FiniteF3(const DirectX::XMFLOAT3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// 実質「消灯」しているか（強度 0 か色が真っ黒）。ミュートしたまま保存された灯を拾う。
bool ColorIsBlack(const DirectX::XMFLOAT3& c)
{
    return (std::max)((std::max)(c.x, c.y), c.z) <= 0.001f;
}

float MaxAbs3(float a, float b, float c)
{
    return (std::max)((std::max)(std::fabs(a), std::fabs(b)), std::fabs(c));
}

// ファイルを丸ごと読む。maxBytes を超えるものは対象外（false）。
bool ReadWholeFile(const fs::path& p, std::string& out, uintmax_t maxBytes)
{
    std::error_code ec;
    const uintmax_t sz = fs::file_size(p, ec);
    if (ec || sz > maxBytes) return false;

    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<size_t>(sz), '\0');
    if (sz > 0 && !f.read(out.data(), static_cast<std::streamsize>(sz))) return false;
    return true;
}

// settings.json（= <プロジェクト>/settings.json。Application::PersistPath のエディタ側と同じ場所）
// から数値キーを 1 つ読む。実行中の値そのものではなく「保存されている設定」を見る。
// 見つからなければ def。
double ReadPersistSetting(const char* key, double def)
{
    const fs::path p = fs::path(PathResolver::AssetsDir()).parent_path() / "settings.json";
    std::error_code ec;
    if (!fs::exists(p, ec)) return def;

    std::ifstream f(p);
    if (!f) return def;
    try
    {
        const nlohmann::json j = nlohmann::json::parse(f);
        auto it = j.find(key);
        if (it != j.end() && it->is_number()) return it->get<double>();
    }
    catch (const std::exception&)
    {
        return def;   // 壊れた settings.json は Application 側も既定値で動くので、ここでも既定値
    }
    return def;
}

// ---- Lua の最小字句スキャン（構文の“閉じ忘れ”だけを実行せずに拾う）----
//
// Lua をリンクせずに本物のパーサ相当の精度が要るのは「文字列 / 長括弧 / コメントを
// 正しく飛ばすこと」だけ。そこさえ間違えなければ end・until・括弧の対応数え上げは
// 誤検知しない（式の文法は見ない＝拾えないものは黙って通す方針）。

struct LuaScanResult
{
    bool        ok   = true;
    int         line = 0;    // 0 = 行を特定できない
    std::string what;        // ok なら空
};

bool IsLuaWordChar(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

// s[i] が長括弧の開き（"[", "[=[", "[==[" …）ならその level を返す。違えば -1。
int LongBracketLevel(const std::string& s, size_t i)
{
    if (i >= s.size() || s[i] != '[') return -1;
    size_t j = i + 1;
    int    level = 0;
    while (j < s.size() && s[j] == '=') { ++level; ++j; }
    return (j < s.size() && s[j] == '[') ? level : -1;
}

// s[i] を長括弧の開きとして、対応する閉じの直後まで i を進める。閉じていなければ false。
bool SkipLongBracket(const std::string& s, size_t& i, int level, int& line)
{
    const size_t n = s.size();
    i += static_cast<size_t>(level) + 2;   // '[' + '='*level + '['
    while (i < n)
    {
        if (s[i] == '\n') { ++line; ++i; continue; }
        if (s[i] == ']')
        {
            size_t j  = i + 1;
            int    lv = 0;
            while (j < n && s[j] == '=') { ++lv; ++j; }
            if (lv == level && j < n && s[j] == ']') { i = j + 1; return true; }
        }
        ++i;
    }
    return false;
}

LuaScanResult ScanLuaSource(const std::string& s)
{
    LuaScanResult r;
    auto fail = [&r](int ln, std::string what) { r.ok = false; r.line = ln; r.what = std::move(what); };

    int line       = 1;
    int blockDepth = 0;   // function / if / do  ←→ end
    int repeatDep  = 0;   // repeat ←→ until
    int paren = 0, brace = 0, bracket = 0;
    int lastOpenLine = 1;

    const size_t n = s.size();
    size_t       i = 0;
    while (i < n)
    {
        const char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }

        // コメント（"--" 行末まで / "--[[ ]]" 長コメント）
        if (c == '-' && i + 1 < n && s[i + 1] == '-')
        {
            const int lv = LongBracketLevel(s, i + 2);
            if (lv >= 0)
            {
                const int startLine = line;
                i += 2;
                if (!SkipLongBracket(s, i, lv, line))
                {
                    fail(startLine, "長コメント --[[ が閉じていない");
                    return r;
                }
                continue;
            }
            while (i < n && s[i] != '\n') ++i;
            continue;
        }

        // 長括弧文字列（Lua は "a[[x]]" も長括弧文字列として読むので、ここの判定順で正しい）
        if (c == '[')
        {
            const int lv = LongBracketLevel(s, i);
            if (lv >= 0)
            {
                const int startLine = line;
                if (!SkipLongBracket(s, i, lv, line))
                {
                    fail(startLine, "長括弧文字列 [[ が閉じていない");
                    return r;
                }
                continue;
            }
            ++bracket; ++i; continue;
        }
        if (c == ']') { --bracket; ++i; continue; }

        // 短い文字列（Lua は生の改行を許さない＝行をまたいだら閉じ忘れ）
        if (c == '"' || c == '\'')
        {
            const int  startLine = line;
            const char quote     = c;
            bool       closed    = false;
            ++i;
            while (i < n)
            {
                const char d = s[i];
                if (d == '\\')
                {
                    if (i + 1 < n && s[i + 1] == '\n') ++line;   // 行継続
                    i += 2;
                    continue;
                }
                if (d == '\n') break;
                ++i;
                if (d == quote) { closed = true; break; }
            }
            if (!closed) { fail(startLine, "文字列が閉じていない"); return r; }
            continue;
        }

        if (c == '(') { ++paren; ++i; continue; }
        if (c == ')') { --paren; ++i; continue; }
        if (c == '{') { ++brace; ++i; continue; }
        if (c == '}') { --brace; ++i; continue; }

        // 数値リテラル（0x1e の "e" 等をキーワード扱いしないため、語より先に食う）
        if (std::isdigit(static_cast<unsigned char>(c)) != 0)
        {
            while (i < n && (IsLuaWordChar(s[i]) || s[i] == '.')) ++i;
            continue;
        }

        // 語（キーワード判定は完全一致。"bend" の "end" を拾わない）
        if (IsLuaWordChar(c))
        {
            const size_t start = i;
            while (i < n && IsLuaWordChar(s[i])) ++i;
            const std::string w = s.substr(start, i - start);
            // for / while は必ず do を伴うので do だけ数える（二重計上しない）
            if (w == "function" || w == "if" || w == "do") { ++blockDepth; lastOpenLine = line; }
            else if (w == "end")
            {
                if (--blockDepth < 0)
                {
                    fail(line, "end が多い（対応する function / if / do が無い）");
                    return r;
                }
            }
            else if (w == "repeat") { ++repeatDep; lastOpenLine = line; }
            else if (w == "until")
            {
                if (--repeatDep < 0) { fail(line, "until が多い（対応する repeat が無い）"); return r; }
            }
            continue;
        }

        ++i;
    }

    if (blockDepth > 0)
    {
        fail(lastOpenLine, "end が " + std::to_string(blockDepth) + " 個足りない");
        return r;
    }
    if (repeatDep > 0) { fail(lastOpenLine, "repeat に対応する until が無い"); return r; }

    auto imbalance = [&fail](int v, const char* open, const char* close) -> bool {
        if (v == 0) return false;
        fail(0, std::string(v > 0 ? close : open) + " が " + std::to_string(std::abs(v))
                + " 個足りない（" + open + close + " の対応が合わない）");
        return true;
    };
    if (imbalance(paren,   "(", ")")) return r;
    if (imbalance(brace,   "{", "}")) return r;
    if (imbalance(bracket, "[", "]")) return r;
    return r;
}

} // namespace

// ===================== DeepDiagReport =====================

void DeepDiagReport::Add(int level, std::string text)
{
    if (level >= 0 && level <= 2) ++levelCounts[level];   // 表示を打ち切っても件数は正確に数える

    if (static_cast<int>(issues.size()) >= kMaxIssues) { ++omitted; return; }
    issues.push_back({level, std::move(text)});
}

int DeepDiagReport::Count(int level) const
{
    return (level >= 0 && level <= 2) ? levelCounts[level] : 0;
}

std::string DeepDiagReport::Summary() const
{
    std::string s = std::to_string(checked) + " 件検査 / エラー " + std::to_string(Errors())
                  + " / 注意 " + std::to_string(Warnings());
    if (omitted > 0) s += " (表示は先頭 " + std::to_string(kMaxIssues) + " 件。他 "
                        + std::to_string(omitted) + " 件省略)";
    if (!skipped.empty()) s += " ※" + skipped;
    return s;
}

// ===================== シェーダー =====================

DeepDiagReport DeepDiag::Shaders()
{
    DeepDiagReport r;
    r.title = "シェーダー";

    const fs::path csoDir(PathResolver::ShaderDirW());
    const fs::path srcDir(PathResolver::ShaderSourceDirW());

    std::error_code ec;
    if (!fs::exists(csoDir, ec))
    {
        r.Add(2, "コンパイル済みシェーダーの置き場が見つからない: " + csoDir.string());
        return r;
    }

    // .hlsli の更新時刻を先に集めておく（複数の .hlsl から共有されるので毎回 stat しない）
    std::map<std::string, fs::file_time_type> depTimes;
    for (const ShaderSource& src : AllShaderSources())
        for (const char* dep : src.staticDeps)
        {
            if (depTimes.count(dep)) continue;
            const fs::path p = srcDir / dep;
            std::error_code e2;
            const auto t = fs::last_write_time(p, e2);
            if (!e2) depTimes[dep] = t;
        }

    int missingSrc = 0;   // 配布版ではソースを同梱しない＝1件ずつ出すと埋まるのでまとめる
    for (const ShaderSource& src : AllShaderSources())
    {
        // このソースの「実質の最終更新」= .hlsl 自身と全 .hlsli のうち一番新しいもの
        bool               haveSrcTime = false;
        fs::file_time_type srcTime{};
        {
            std::error_code e2;
            const auto t = fs::last_write_time(srcDir / src.relPath, e2);
            if (!e2) { srcTime = t; haveSrcTime = true; }
            else      ++missingSrc;
        }
        for (const char* dep : src.staticDeps)
        {
            auto it = depTimes.find(dep);
            if (it == depTimes.end()) continue;
            if (!haveSrcTime || it->second > srcTime) { srcTime = it->second; haveSrcTime = true; }
        }

        for (const ShaderVariant& v : src.variants)
        {
            ++r.checked;
            const fs::path cso = csoDir / fs::path(std::wstring(v.csoName));
            const std::string csoName = cso.filename().string();

            std::error_code e2;
            if (!fs::exists(cso, e2))
            {
                r.Add(2, csoName + " が無い（" + src.relPath + " のコンパイルに失敗している）");
                continue;
            }

            uintmax_t size = 0;
            if (!CsoLooksValid(cso, size))
            {
                r.Add(2, csoName + " が壊れている（サイズ " + std::to_string(size)
                         + " バイト / DXBC ヘッダ不正）。シェーダーを再ビルドすること");
                continue;
            }

            if (haveSrcTime)
            {
                const auto csoTime = fs::last_write_time(cso, e2);
                if (!e2 && csoTime < srcTime)
                    r.Add(1, csoName + " が " + src.relPath
                             + " より古い。編集した内容が反映されていない（要リビルド）");
            }
        }
    }

    if (missingSrc > 0)
        r.Add(0, std::to_string(missingSrc) + " 個の .hlsl ソースが見つからない（配布版なら正常。"
                 "鮮度チェックはそのぶんスキップした）");

    // プロジェクト側の自作シェーダー（assets/shaders/*.hlsl）のコンパイル結果
    ShaderManager* sm = ShaderManager::Instance();
    if (sm == nullptr)
    {
        r.Add(0, "実行時シェーダーコンパイラが無効（ゲームモード）。自作シェーダーは検査していない");
        return r;
    }

    const fs::path projShaderDir(PathResolver::ProjectShaderDir());
    for (const fs::path& hlsl : CollectFiles(projShaderDir, {".hlsl"}))
    {
        // プロジェクト assets/shaders 相対・スラッシュ区切りへ正規化
        const std::string rel = RelDisplay(hlsl, projShaderDir);
        if (rel.empty() || rel == hlsl.string()) continue;   // 相対化できない＝別ボリューム等

        // エンジンシェーダーの上書き（Registry に同じ相対パスがある）はカスタム扱いではない。
        // 失敗ログは ShaderManager 側に残らないのでここでは判定できない。
        if (FindShaderSourceByRelPath(rel) != nullptr) continue;

        ++r.checked;
        if (sm->HasValidCustomShader(rel)) continue;

        std::string err = sm->GetCustomShaderError(rel);
        const size_t nl = err.find('\n');
        if (nl != std::string::npos) err = err.substr(0, nl);
        r.Add(2, "自作シェーダー " + rel + " がコンパイルできない: "
                 + (err.empty() ? "(詳細は dx12_engine.log)" : err));
    }

    return r;
}

// ===================== テクスチャ / ガンマ（保存形式側）=====================

DeepDiagReport DeepDiag::Textures()
{
    DeepDiagReport r;
    r.title = "テクスチャ";

    const fs::path root(PathResolver::AssetsDir());
    std::vector<fs::path> files =
        CollectFiles(root, {".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr"});

    if (files.empty())
    {
        r.Add(0, "assets 配下に画像が見つからない: " + root.string());
        return r;
    }

    const int total = static_cast<int>(files.size());
    if (total > kMaxTextures)
    {
        files.resize(kMaxTextures);
        r.skipped = "全 " + std::to_string(total) + " 枚のうち先頭 "
                  + std::to_string(kMaxTextures) + " 枚だけ検査した";
    }

    int noMip = 0, huge = 0;
    for (const fs::path& p : files)
    {
        ++r.checked;
        const std::string rel = RelDisplay(p, root);

        const TextureProbeInfo info = TextureLoader::Probe(p.wstring());
        if (!info.ok)
        {
            r.Add(2, rel + " が読み込めない: " + (info.error.empty() ? "原因不明" : info.error));
            continue;
        }
        if (info.width == 0 || info.height == 0)
        {
            r.Add(2, rel + " のサイズが 0（壊れているか未対応形式）");
            continue;
        }

        // ── ガンマ ──
        // エンジンは「アルベドは sRGB として読む / 法線・metalRoughness はリニアで読む」
        // という *読み込み時の指定* で色空間を決めている（フォーマットには持たせていない）。
        // ファイル側が最初から _SRGB フォーマットだと MakeSRGB が効かず、
        // 法線・データ系マップが sRGB デコードされてライティングが確実に狂う。
        if (IsSrgbFormatName(info.format))
        {
            if (LooksLikeNormalMap(p))
                r.Add(2, rel + " は法線マップだが " + info.format
                         + " で保存されている。リニア(_UNORM)で保存し直すこと");
            else if (LooksLikeDataMap(p))
                r.Add(2, rel + " はデータ系マップだが " + info.format
                         + " で保存されている。リニア(_UNORM)で保存し直すこと");
        }

        if (info.mipLevels <= 1 && (std::max)(info.width, info.height) >= 512) ++noMip;
        if ((std::max)(info.width, info.height) > 4096) ++huge;
    }

    if (noMip > 0)
        r.Add(0, std::to_string(noMip) + " 枚がミップ無しの大きな画像。"
                 "縮小時にちらつく場合はミップ付き(.dds)にする");
    if (huge > 0)
        r.Add(1, std::to_string(huge) + " 枚が 4096px 超。VRAM とロード時間を食う");

    return r;
}

// ===================== モデル =====================

DeepDiagReport DeepDiag::Models()
{
    DeepDiagReport r;
    r.title = "モデル";

    const fs::path root(PathResolver::AssetsDir());
    std::vector<fs::path> files = CollectFiles(root, {".gltf", ".glb", ".fbx", ".obj", ".dae"});

    if (files.empty())
    {
        r.Add(0, "assets 配下にモデルが見つからない: " + root.string());
        return r;
    }

    const int total = static_cast<int>(files.size());
    if (total > kMaxModels)
    {
        files.resize(kMaxModels);
        r.skipped = "全 " + std::to_string(total) + " 個のうち先頭 "
                  + std::to_string(kMaxModels) + " 個だけ検査した";
    }

    for (const fs::path& p : files)
    {
        ++r.checked;
        const std::string rel = RelDisplay(p, root);

        const ModelProbeInfo info = ModelLoader::Probe(p);
        if (!info.ok)
        {
            r.Add(2, rel + " が読み込めない: " + (info.error.empty() ? "原因不明" : info.error));
            continue;
        }
        if (info.meshCount == 0 || info.totalVertices == 0)
        {
            r.Add(2, rel + " に描画できるメッシュが無い（メッシュ " + std::to_string(info.meshCount)
                     + " / 頂点 " + std::to_string(info.totalVertices) + "）");
            continue;
        }

        if (!Finite3(info.aabbMin) || !Finite3(info.aabbMax))
        {
            r.Add(2, rel + " の頂点に NaN/Inf が含まれている（描画すると画面が壊れる）");
            continue;
        }

        const float ex = info.aabbMax[0] - info.aabbMin[0];
        const float ey = info.aabbMax[1] - info.aabbMin[1];
        const float ez = info.aabbMax[2] - info.aabbMin[2];
        const float maxExtent = (std::max)({ex, ey, ez});
        if (maxExtent <= 0.0f)
            r.Add(1, rel + " の AABB が潰れている（全頂点が同一点）。ピッキングとフォーカスが効かない");
        else if (maxExtent > 5000.0f)
            r.Add(1, rel + " が極端に大きい（" + std::to_string(static_cast<int>(maxExtent))
                     + " ユニット）。単位系(cm/m)が合っていない可能性");

        // SkinningBuffer は超過分を無言で切り捨てる＝一部のボーンが動かない絵になる
        if (info.boneCount > kMaxBonesRef)
            r.Add(2, rel + " のボーン数が " + std::to_string(info.boneCount) + " で上限 "
                     + std::to_string(kMaxBonesRef) + " を超えている。超過分は無視され変形が壊れる");

        if (info.hasSkeleton && info.animations.empty())
            r.Add(0, rel + " はスケルトンを持つがアニメーションが 0 本");
    }

    return r;
}

// ===================== ガンマ（表示パイプライン側）=====================

DeepDiagReport DeepDiag::Gamma(Application& app)
{
    DeepDiagReport r;
    r.title = "ガンマ / 表示パイプライン";

    const Application::DiagRenderInfo info = app.GetDiagRenderInfo();

    // ① シーン RT はリニア HDR でなければならない。
    //    ここが UNORM だとライティング計算がクランプされ、ブルーム/露出が破綻する。
    ++r.checked;
    if (info.sceneColorFormat != 10 /*R16G16B16A16_FLOAT*/)
        r.Add(2, "シーン RT が " + FormatName(info.sceneColorFormat)
                 + "。リニア HDR (R16G16B16A16_FLOAT) 前提の計算と合っていない");

    // ② バックバッファは *_SRGB であってはならない。
    //    PostProcess の最終段が pow(1/2.2) 済みの値を書くので、
    //    RTV 側でもう一度 sRGB エンコードされると二重ガンマ（全体が白っぽく浅くなる）。
    ++r.checked;
    if (IsSrgbFormatName(FormatName(info.backBufferFormat)))
        r.Add(2, "バックバッファが " + FormatName(info.backBufferFormat)
                 + "。PostProcess が既にガンマを掛けているためガンマ二重適用になる");

    ++r.checked;
    if (info.depthFormat != 40 /*D32_FLOAT*/)
        r.Add(0, "深度バッファが " + FormatName(info.depthFormat) + "（想定は D32_FLOAT）");

    ++r.checked;
    if (info.tonemapper < 0 || info.tonemapper > 2)
        r.Add(2, "トーンマップ設定が範囲外 (" + std::to_string(info.tonemapper) + ")。シーン JSON が壊れている");

    // ③ 参考情報。スクリーンショットは この設定を CPU 側で再現して書き出す。
    static const char* kToneNames[] = { "ACES", "AgX", "なし(ガンマのみ)" };
    if (info.tonemapper >= 0 && info.tonemapper <= 2)
        r.Add(0, std::string("トーンマップ: ") + kToneNames[info.tonemapper]
                 + " / ポスト処理: " + (info.postEnabled ? "有効" : "無効")
                 + " / 露出: " + (info.exposureOn ? std::to_string(info.exposure) : "既定"));

    return r;
}

// ===================== シーンのアセット =====================

DeepDiagReport DeepDiag::SceneAssets(Application& app)
{
    DeepDiagReport r;
    r.title = "シーンのアセット";

    Scene* scene = app.GetScene();
    if (scene == nullptr)
    {
        r.Add(2, "シーンが読み込まれていない");
        return r;
    }
    entt::registry& reg = scene->GetRegistry();

    auto nameOf = [&reg](entt::entity e) -> std::string {
        if (reg.all_of<NameTag>(e)) return reg.get<NameTag>(e).name;
        return "entity#" + std::to_string(static_cast<uint32_t>(e));
    };

    auto checkFile = [&r](const std::string& rel, const std::string& who, const char* kind) {
        fs::path abs;
        if (!ResolveAssetPath(rel, abs)) return;
        std::error_code ec;
        if (!fs::exists(abs, ec))
            r.Add(2, who + " の" + kind + " が見つからない: " + rel);
    };

    for (auto [e, t] : reg.view<Transform>().each())
    {
        ++r.checked;
        const std::string who = nameOf(e);

        // NaN が 1 つでも入ると行列が全部壊れ、そのフレーム以降ギズモもピッキングも効かなくなる
        const float vals[9] = { t.position.x, t.position.y, t.position.z,
                                t.rotation.x, t.rotation.y, t.rotation.z,
                                t.scale.x,    t.scale.y,    t.scale.z };
        bool bad = false;
        for (float v : vals) if (!std::isfinite(v)) bad = true;
        if (bad)
        {
            r.Add(2, who + " の Transform に NaN/Inf が入っている。ギズモとピッキングが効かなくなる");
            continue;
        }

        if (t.scale.x == 0.0f || t.scale.y == 0.0f || t.scale.z == 0.0f)
            r.Add(1, who + " のスケールに 0 が入っている（潰れて見えない）");

        if (t.parent != entt::null && !reg.valid(t.parent))
            r.Add(2, who + " の親エンティティが存在しない（階層が壊れている）");
    }

    for (auto [e, mr] : reg.view<MeshRenderer>().each())
    {
        const std::string who = nameOf(e);

        checkFile(mr.modelPath, who, "モデル");

        // パスが正しくてもロードに失敗していれば meshes は空＝画面に何も出ない。
        // 「配置したのに映らない」の大半はここ。
        if (mr.meshes.empty())
            r.Add(2, who + " にメッシュがロードされていない（modelPath=" + mr.modelPath + "）");
        else if (mr.materials.empty())
            r.Add(1, who + " にマテリアルが無い。既定の白テクスチャで描かれる");

        for (const std::string& p : mr.overrideAlbedoTexture)        checkFile(p, who, "アルベド");
        for (const std::string& p : mr.overrideNormalTexture)        checkFile(p, who, "法線マップ");
        for (const std::string& p : mr.overrideMetalRoughnessTexture)checkFile(p, who, "metalRoughness");
        for (const std::string& p : mr.materialAsset)                checkFile(p, who, "マテリアル");
        if (!mr.shaderPath.empty())
        {
            if (ShaderManager* sm = ShaderManager::Instance())
                if (!sm->HasValidCustomShader(mr.shaderPath))
                    r.Add(2, who + " に割り当てたシェーダーが有効でない: " + mr.shaderPath);
        }
    }

    for (auto [e, sp] : reg.view<Sprite2D>().each())
    {
        const std::string who = nameOf(e);
        if (sp.texturePath.empty())
            r.Add(1, who + " のスプライトにテクスチャが設定されていない");
        else
            checkFile(sp.texturePath, who, "スプライト画像");

        if (sp.size.x <= 0.0f || sp.size.y <= 0.0f)
            r.Add(1, who + " のスプライトサイズが 0 以下（表示されない）");
        if (sp.color.w <= 0.0f)
            r.Add(1, who + " のスプライトが完全に透明（color.a=0）");
    }

    return r;
}

// ===================== ライティング =====================

DeepDiagReport DeepDiag::Lighting(Application& app)
{
    DeepDiagReport r;
    r.title = "ライティング";

    Scene* scene = app.GetScene();
    if (scene == nullptr)
    {
        r.Add(2, "シーンが読み込まれていない");
        return r;
    }
    entt::registry& reg = scene->GetRegistry();

    int dirCount = 0, pointCount = 0, spotCount = 0;
    int pointShadow = 0, spotShadow = 0;
    int droppedNoTransform = 0;

    // ---- 平行光（太陽）----
    // Application は「先頭の 1 灯」しか読まない。2 灯目以降は存在すら気付けない。
    for (auto [e, dl] : reg.view<DirectionalLight>().each())
    {
        ++r.checked;
        ++dirCount;
        const std::string who = EntityLabel(reg, e);

        if (!FiniteF3(dl.direction) || !FiniteF3(dl.color)
            || !Finite1(dl.intensity) || !Finite1(dl.ambient))
        {
            r.Add(2, who + " の平行光に NaN/Inf が入っている（画面全体のライティングが壊れる）");
            continue;
        }
        if (MaxAbs3(dl.direction.x, dl.direction.y, dl.direction.z) <= 0.0f)
            r.Add(2, who + " の平行光の向きが (0,0,0)。正規化できず、光と影の向きが不定になる");
        if (dl.intensity <= 0.0f || ColorIsBlack(dl.color))
            r.Add(1, who + " の平行光が消灯している（強度 " + Fmt(dl.intensity)
                     + " / 色が真っ黒）。ミュートしたまま保存していないか");
        if (dl.ambient < 0.0f)
            r.Add(2, who + " の ambient が負（" + Fmt(dl.ambient) + "）。影の側が黒く潰れる");
        else if (dl.ambient > 1.0f)
            r.Add(1, who + " の ambient が " + Fmt(dl.ambient)
                     + " と大きい。影が浮いてコントラストが出ない");
    }

    // ---- ポイントライト ----
    // Application の収集は view<PointLight, Transform> なので、Transform が無い灯は
    // 数にも入らず完全に無視される（インスペクタ上は存在するのに光らない）。
    for (auto [e, pl] : reg.view<PointLight>().each())
    {
        ++r.checked;
        const std::string who = EntityLabel(reg, e);

        if (!reg.all_of<Transform>(e))
        {
            ++droppedNoTransform;
            r.Add(2, who + " のポイントライトに Transform が無い。位置が決まらず GPU へ送られない（完全に無視される）");
            continue;
        }
        ++pointCount;
        if (pl.castShadows) ++pointShadow;

        if (!FiniteF3(pl.color) || !Finite1(pl.intensity) || !Finite1(pl.range))
        {
            r.Add(2, who + " のポイントライトに NaN/Inf が入っている");
            continue;
        }
        if (pl.range <= 0.0f)
            r.Add(2, who + " のポイントライトの range が " + Fmt(pl.range)
                     + "。届く距離が 0 なので何も照らさない");
        else if (pl.range > 10000.0f)
            r.Add(1, who + " のポイントライトの range が " + Fmt(pl.range, 0)
                     + " と極端に大きい。実質シーン全体を照らして減衰が効かない");
        if (pl.intensity <= 0.0f || ColorIsBlack(pl.color))
            r.Add(1, who + " のポイントライトが消灯している（強度 " + Fmt(pl.intensity)
                     + " / 色が真っ黒）。ミュートしたまま保存していないか");
    }

    // ---- スポットライト ----
    for (auto [e, sl] : reg.view<SpotLight>().each())
    {
        ++r.checked;
        const std::string who = EntityLabel(reg, e);

        if (!reg.all_of<Transform>(e))
        {
            ++droppedNoTransform;
            r.Add(2, who + " のスポットライトに Transform が無い。位置が決まらず GPU へ送られない（完全に無視される）");
            continue;
        }
        ++spotCount;
        if (sl.castShadows) ++spotShadow;

        if (!FiniteF3(sl.color) || !FiniteF3(sl.direction)
            || !Finite1(sl.intensity) || !Finite1(sl.range)
            || !Finite1(sl.innerConeDeg) || !Finite1(sl.outerConeDeg))
        {
            r.Add(2, who + " のスポットライトに NaN/Inf が入っている");
            continue;
        }
        if (MaxAbs3(sl.direction.x, sl.direction.y, sl.direction.z) <= 0.0f)
            r.Add(2, who + " のスポットライトの向きが (0,0,0)。円錐の軸が決まらない");
        if (sl.range <= 0.0f)
            r.Add(2, who + " のスポットライトの range が " + Fmt(sl.range)
                     + "。届く距離が 0 なので何も照らさない");
        if (sl.intensity <= 0.0f || ColorIsBlack(sl.color))
            r.Add(1, who + " のスポットライトが消灯している（強度 " + Fmt(sl.intensity)
                     + " / 色が真っ黒）。ミュートしたまま保存していないか");
        // 描画側は outer = max(outer, inner) に補正する＝内外が逆だと
        // 「エラーにならないままフォールオフだけ消えて縁がくっきりする」。
        if (sl.innerConeDeg > sl.outerConeDeg)
            r.Add(1, who + " のスポットライトの内コーン角(" + Fmt(sl.innerConeDeg, 1)
                     + "°) が外コーン角(" + Fmt(sl.outerConeDeg, 1)
                     + "°) より大きい。描画側は外=内へ補正するのでフォールオフが消えて縁が硬くなる");
        if (sl.outerConeDeg <= 0.0f)
            r.Add(2, who + " のスポットライトの外コーン角が " + Fmt(sl.outerConeDeg, 1)
                     + "°。円錐が閉じていて何も照らさない");
        else if (sl.outerConeDeg >= 180.0f)
            r.Add(1, who + " のスポットライトの外コーン角が " + Fmt(sl.outerConeDeg, 1)
                     + "°。全方向に広がり、実質ポイントライトになっている");
    }

    // ---- 灯数の予算（超過分は無言で落ちるので必ず出す）----
    if (dirCount == 0)
        r.Add(1, "DirectionalLight（太陽）が 1 つも無い。既定の太陽へのフォールバックは無いので、"
                 "点/スポット以外は環境光だけの平坦な絵になる");
    else if (dirCount >= 2)
        // 予備の太陽を置いておく運用は普通にあるので注意止まり（エラーにすると UI 自動テストが
        // 途中でライトを生やした時点で診断全体が落ちる）。
        r.Add(1, "DirectionalLight が " + std::to_string(dirCount)
                 + " 個ある。実際に使われるのは先頭の 1 灯だけで、残りは無言で無視される"
                   "（要らない方を消すか、影響を確認すること）");

    // クラスタードライティング（Forward+）: 個別上限は無く、point + spot の合計で判定する。
    const int punctualCount = pointCount + spotCount;
    if (punctualCount > kMaxTotalLightsRef)
        r.Add(2, "ライト（点+スポット）が " + std::to_string(punctualCount) + " 灯ある。GPU へ送れるのは先頭 "
                 + std::to_string(kMaxTotalLightsRef) + " 灯だけで、残り "
                 + std::to_string(punctualCount - kMaxTotalLightsRef)
                 + " 灯は無言で描画されない（灯を減らすか range を広げてまとめること）");
    else if (punctualCount > kMaxPerClusterRef)
        // 1 クラスタ 128 灯の切り捨ては GPU 側でしか分からないので、注意止まりで誘導する。
        r.Add(1, "ライト（点+スポット）が " + std::to_string(punctualCount) + " 灯ある。合計の上限 "
                 + std::to_string(kMaxTotalLightsRef) + " 灯には収まっているが、画面を割った"
                   "クラスタ 1 マスで評価できるのは " + std::to_string(kMaxPerClusterRef)
                 + " 灯まで。密集した所は無言で切り捨てられるので、"
                   "「ツール > ライティング > クラスタデバッグ表示 > ライト複雑度」で"
                   "白く飽和している場所が無いか確認すること");

    if (droppedNoTransform > 0)
        r.Add(1, "Transform を持たないライトが " + std::to_string(droppedNoTransform)
                 + " 灯ある。上限の数にも入らず、まるごと無視される");

    // ---- 影スロット ----
    if (pointShadow > kMaxShadowPointRef)
        r.Add(1, "影を落とすポイントライトが " + std::to_string(pointShadow) + " 灯（影スロットは "
                 + std::to_string(kMaxShadowPointRef)
                 + " 枠）。カメラに近い順で選ばれ、あぶれた灯は光るのに影だけ出ない");
    if (spotShadow > kMaxShadowSpotRef)
        r.Add(1, "影を落とすスポットライトが " + std::to_string(spotShadow) + " 灯（影スロットは "
                 + std::to_string(kMaxShadowSpotRef)
                 + " 枠）。カメラに近い順で選ばれ、あぶれた灯は光るのに影だけ出ない");
    if (!scene->GetShadowsEnabled() && (pointShadow > 0 || spotShadow > 0 || dirCount > 0))
        r.Add(1, "シーンの影が OFF（shadowsEnabled=false）。castShadows を立てたライトがあっても影は一切出ない");

    // ---- IBL（金属は映り込みが全て）----
    ++r.checked;
    const SkyboxSettings& sky = scene->GetSkyboxSettings();
    if (sky.envMapPath.empty())
    {
        int metalish = 0;
        for (auto [e, mr] : reg.view<MeshRenderer>().each())
        {
            (void)e;
            if (mr.overrideMetallic >= 0.5f) ++metalish;
        }
        if (metalish >= 3)
            r.Add(1, "環境マップ(IBL)が未設定なのに metallic 0.5 以上のマテリアルが "
                     + std::to_string(metalish)
                     + " 個ある。金属は環境の映り込みが見た目の全てなので、このままだとほぼ真っ黒に見える");
    }
    else
    {
        fs::path abs;
        if (ResolveAssetPath(sky.envMapPath, abs))
        {
            std::error_code ec;
            if (!fs::exists(abs, ec))
                r.Add(2, "環境マップ(IBL)が見つからない: " + sky.envMapPath
                         + "（IBL が焼けず、金属と反射が死ぬ）");
        }
        if (sky.iblIntensity <= 0.0f)
            r.Add(1, "環境マップは設定されているが IBL 強度が " + Fmt(sky.iblIntensity)
                     + "。IBL が効いていない");
    }

    return r;
}

// ===================== ハイトフィールド地形 =====================
// ※ 関数名 Terrain がコンポーネント dx12e::Terrain を隠すので、
//    この関数の中でコンポーネントを指すときは TerrainComp を使う。

DeepDiagReport DeepDiag::Terrain(Application& app)
{
    DeepDiagReport r;
    r.title = "地形";

    Scene* scene = app.GetScene();
    if (scene == nullptr)
    {
        r.Add(2, "シーンが読み込まれていない");
        return r;
    }
    entt::registry& reg = scene->GetRegistry();

    std::map<std::string, std::string> pathOwner;   // heightmapPath → 最初に使ったエンティティ名
    int terrainCount = 0;

    for (auto [e, t] : reg.view<TerrainComp>().each())
    {
        ++r.checked;
        ++terrainCount;
        const std::string who = EntityLabel(reg, e);

        // ---- コンポーネントの値 ----
        // Jolt の HeightFieldShape は「サンプル数がブロックサイズの倍数」を要求する。
        // 満たさないとメッシュだけ出てコリジョンの生成に失敗する＝すり抜ける。
        if (t.resolution < HeightField::kMinResolution || t.resolution > HeightField::kMaxResolution
            || (t.resolution % 4u) != 0u)
            r.Add(2, who + " の解像度が " + std::to_string(t.resolution)
                     + "。Jolt の HeightFieldShape はサンプル数が 4 の倍数（"
                     + std::to_string(HeightField::kMinResolution) + "〜"
                     + std::to_string(HeightField::kMaxResolution)
                     + "）である必要があり、この値ではコリジョンが作られない");
        if (!(t.worldSize > 0.0f))
            r.Add(2, who + " の worldSize が " + Fmt(t.worldSize) + "。地形が潰れて当たり判定も作れない");
        if (!(t.maxHeight > 0.0f))
            r.Add(1, who + " の maxHeight が " + Fmt(t.maxHeight) + "。ブラシの高さクランプが効かない");

        // ---- 読み込み済みの高さ配列 ----
        if (!t._hf)
        {
            r.Add(2, who + " のハイトフィールドが読み込まれていない（描画メッシュもコリジョンも作れない）");
        }
        else if (!t._hf->IsValid())
        {
            r.Add(2, who + " のハイトフィールドが不正（解像度 " + std::to_string(t._hf->Resolution()) + "）");
        }
        else
        {
            if (t._hf->Resolution() != t.resolution)
                r.Add(1, who + " のコンポーネント解像度(" + std::to_string(t.resolution)
                         + ") と読み込み済みハイトフィールド(" + std::to_string(t._hf->Resolution())
                         + ") が食い違う");
            if (std::fabs(t._hf->WorldSize() - t.worldSize) > 0.01f)
                r.Add(1, who + " のコンポーネント worldSize(" + Fmt(t.worldSize)
                         + ") とハイトフィールド(" + Fmt(t._hf->WorldSize()) + ") が食い違う");
        }

        // ---- .hf ファイル ----
        if (t.heightmapPath.empty())
        {
            r.Add(1, who + " のハイトマップが未保存（heightmapPath が空）。"
                           "シーンを開き直すと彫った地形が平坦に戻る");
        }
        else
        {
            auto owner = pathOwner.find(t.heightmapPath);
            if (owner != pathOwner.end())
                r.Add(2, who + " と " + owner->second + " が同じハイトマップ " + t.heightmapPath
                         + " を共有している。片方を彫るともう片方も書き換わる（別名で保存し直すこと）");
            else
                pathOwner.emplace(t.heightmapPath, who);

            fs::path abs;
            std::error_code ec;
            if (!ResolveAssetPath(t.heightmapPath, abs))
            {
                r.Add(2, who + " のハイトマップのパスが不正: " + t.heightmapPath);
            }
            else if (!fs::exists(abs, ec))
            {
                r.Add(2, who + " のハイトマップが見つからない: " + t.heightmapPath
                         + "（シーンを開き直すと平坦な地形になる）");
            }
            else
            {
                // ヘッダ 16B = magic('DXHF') + version + resolution + worldSize
                char          head[HeightField::kHeaderSize] = {};
                std::ifstream hfFile(abs, std::ios::binary);
                const bool    headOk =
                    hfFile && static_cast<bool>(hfFile.read(head, static_cast<std::streamsize>(sizeof(head))));
                if (!headOk)
                {
                    r.Add(2, t.heightmapPath + " が短すぎる（ヘッダ 16 バイトも読めない＝壊れている）");
                }
                else
                {
                    std::uint32_t magic = 0, ver = 0, res = 0;
                    float         ws = 0.0f;
                    std::memcpy(&magic, head +  0, sizeof(magic));
                    std::memcpy(&ver,   head +  4, sizeof(ver));
                    std::memcpy(&res,   head +  8, sizeof(res));
                    std::memcpy(&ws,    head + 12, sizeof(ws));

                    if (magic != HeightField::kMagic)
                    {
                        r.Add(2, t.heightmapPath + " が .hf ではない（先頭 4 バイトが 'DXHF' でない）");
                    }
                    else if (ver != HeightField::kVersion)
                    {
                        r.Add(2, t.heightmapPath + " の版が " + std::to_string(ver)
                                 + "（このエンジンが読めるのは " + std::to_string(HeightField::kVersion)
                                 + " のみ）。読み込みに失敗して平坦になる");
                    }
                    else
                    {
                        const std::uintmax_t want =
                            static_cast<std::uintmax_t>(HeightField::kHeaderSize)
                            + static_cast<std::uintmax_t>(res) * res * sizeof(float);
                        const std::uintmax_t got = fs::file_size(abs, ec);
                        if (!ec && got < want)
                            r.Add(2, t.heightmapPath + " のサイズが足りない（" + std::to_string(got)
                                     + " / 必要 " + std::to_string(want)
                                     + " バイト）。書き出しの途中で落ちた可能性");

                        if (res < HeightField::kMinResolution || res > HeightField::kMaxResolution
                            || (res % 4u) != 0u)
                            r.Add(2, t.heightmapPath + " の解像度 " + std::to_string(res)
                                     + " は読み込み条件（4 の倍数）を満たさない。ロードが失敗して平坦になる");
                        else if (res != t.resolution)
                            r.Add(1, who + " のコンポーネント解像度(" + std::to_string(t.resolution)
                                     + ") と .hf(" + std::to_string(res)
                                     + ") が食い違う。読み込み時は .hf 側が採用される");

                        if (!(ws > 0.0f))
                            r.Add(2, t.heightmapPath + " の worldSize が " + Fmt(ws) + "（不正な値）");
                        else if (std::fabs(ws - t.worldSize) > 0.01f)
                            r.Add(1, who + " のコンポーネント worldSize(" + Fmt(t.worldSize)
                                     + ") と .hf(" + Fmt(ws) + ") が食い違う");
                    }
                }
            }
        }

        // ---- コライダー（PhysicsSystem::RegisterBody が RigidBody を見て作る）----
        if (const RigidBody* rb = reg.try_get<RigidBody>(e))
        {
            if (rb->motionType != MotionType::Static)
                r.Add(1, who + " の RigidBody が Static ではない。地形の HeightFieldShape は静的専用なので、"
                               "指定に関わらず Static として作られる");
            // Play 中なら実体（Jolt のボディ）が本当に作られたかまで見られる。
            // 生成失敗は Logger にしか出ないので、遊んでいる最中は気付けない。
            if (app.GetEngineMode() == Application::EngineMode::Playing
                && rb->bodyId == kInvalidBodyId)
                r.Add(2, who + " の地形コライダーが Play 中に生成されていない"
                               "（高さ配列が不正で HeightFieldShape の生成に失敗した可能性）");
        }
        else
        {
            r.Add(2, who + " に RigidBody が無い。コライダーが作られず地形をすり抜ける");
        }

        // ---- 描画メッシュ ----
        const MeshRenderer* mr = reg.try_get<MeshRenderer>(e);
        if (mr == nullptr || mr->meshes.empty())
            r.Add(2, who + " の地形メッシュが生成されていない（当たり判定はあるのに画面に出ない）");

        // ---- Transform（コライダーは回転もスケールも無視して作られる）----
        if (const Transform* tf = reg.try_get<Transform>(e))
        {
            if (MaxAbs3(tf->rotation.x, tf->rotation.y, tf->rotation.z) > 0.01f)
                r.Add(1, who + " の地形に回転が入っている。当たり判定は回転を無視して作られるので、"
                               "見た目の地面と実際に立てる面がズレる");
            if (std::fabs(tf->scale.x - 1.0f) > 0.001f || std::fabs(tf->scale.y - 1.0f) > 0.001f
                || std::fabs(tf->scale.z - 1.0f) > 0.001f)
                r.Add(1, who + " の地形にスケールが掛かっている。当たり判定は高さ配列そのままなので、"
                               "見た目の地面と実際に立てる面がズレる");
        }
        else
        {
            r.Add(2, who + " の地形に Transform が無い");
        }
    }

    if (terrainCount == 0)
        r.Add(0, "このシーンに地形（Terrain コンポーネント）は無い");

    return r;
}

// ===================== ピッキング =====================

DeepDiagReport DeepDiag::Picking(Application& app)
{
    DeepDiagReport r;
    r.title = "ピッキング";

    Scene* scene = app.GetScene();
    if (scene == nullptr)
    {
        r.Add(2, "シーンが読み込まれていない");
        return r;
    }
    entt::registry& reg = scene->GetRegistry();

    int noCpuCache = 0;   // CPU キャッシュ無し＝三角形判定に落とせない
    int skinnedAabb = 0;  // 仕様どおり AABB 止まり
    int offOrigin = 0;    // ジオメトリが原点から大きくズレている

    // 「スキンかどうか」は描画リストの skin ポインタで判定する（ScenePick と同じ根拠）。
    // ここで reg.all_of<SkeletalAnimation>() を使うと、entt がコンポーネントのストレージを
    // 実体化する際に SkinningBuffer / Animator / Skeleton の完全型を要求する＝
    // Gui へ Animation の実装ヘッダを丸ごと引きずり込むことになるので使わない。
    // 描画リストが空（未描画）のときは全部「非スキン」扱いになるが、
    // スキンメッシュも CPU キャッシュ自体は持つので誤検知にはならない。
    std::set<entt::entity> skinnedSet;
    for (const DrawItem& item : app.GetDrawItems())
        if (item.skin != nullptr) skinnedSet.insert(item.e);

    for (auto [e, tf, mr] : reg.view<Transform, MeshRenderer>().each())
    {
        ++r.checked;
        const std::string who = EntityLabel(reg, e);

        // ワールド行列の逆行列でレイをローカルへ落とすので、行列が作れない時点で
        // クリックしても永久に当たらない（ギズモも掴めない）。
        if (!FiniteF3(tf.position) || !FiniteF3(tf.rotation) || !FiniteF3(tf.scale))
        {
            r.Add(2, who + " の Transform に NaN/Inf が入っている。ワールド行列の逆行列が作れず、"
                           "クリックしても選択できない");
            continue;
        }
        if (tf.scale.x == 0.0f || tf.scale.y == 0.0f || tf.scale.z == 0.0f)
            r.Add(2, who + " のスケールに 0 がある。逆行列が作れずレイが素通りする（クリックで選択できない）");

        const bool isSkinned = (skinnedSet.count(e) != 0);

        for (size_t mi = 0; mi < mr.meshes.size(); ++mi)
        {
            const Mesh* mesh = mr.meshes[mi];
            if (mesh == nullptr)
            {
                r.Add(2, who + " の meshes[" + std::to_string(mi) + "] が null（描画もピックもできない）");
                continue;
            }

            const DirectX::XMFLOAT3 mn = mesh->GetAABBMin();
            const DirectX::XMFLOAT3 mx = mesh->GetAABBMax();
            if (!FiniteF3(mn) || !FiniteF3(mx))
            {
                r.Add(2, who + " のメッシュ AABB が NaN。カリングもピッキングも成立しない");
                continue;
            }

            const float extent = (std::max)((std::max)(mx.x - mn.x, mx.y - mn.y), mx.z - mn.z);
            if (extent > 1.0e5f)
                r.Add(2, who + " のメッシュ AABB が異常に大きい（" + Fmt(extent, 0)
                         + " ユニット）。フラスタムカリングが常に通り、ピック候補も絞れなくなる");

            if (isSkinned)
            {
                // スキンメッシュは頂点が GPU 側で変形するので、CPU の三角形判定は原理的に合わない
                ++skinnedAabb;
                continue;
            }

            if (mesh->GetIndices().size() < 3u || mesh->GetPositions().empty())
                ++noCpuCache;

            // AABB の中心が原点から大きく離れたモデル（bake で位置が焼き込まれた glb 等）。
            // 描画リストは中心基準の球を使うようになったが、LOD 選択と手動のフォーカスは
            // 依然ここでズレやすい既知の形なので出しておく。
            const float cx = 0.5f * (mn.x + mx.x);
            const float cy = 0.5f * (mn.y + mx.y);
            const float cz = 0.5f * (mn.z + mx.z);
            const float off = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (extent > 0.0f && off > 50.0f && off > extent * 10.0f)
                ++offOrigin;
        }
    }

    if (noCpuCache > 0)
        r.Add(1, std::to_string(noCpuCache)
                 + " 個のメッシュが CPU 側のインデックス/頂点キャッシュを持たない。"
                   "三角形の精密判定ができず AABB 判定に落ちる（隙間の多い形では背後の物を掴めない）");
    if (skinnedAabb > 0)
        r.Add(0, "スキンメッシュ " + std::to_string(skinnedAabb)
                 + " 個は仕様どおり AABB 判定（頂点が GPU 側で変形するため三角形判定はできない）");
    if (offOrigin > 0)
        r.Add(1, std::to_string(offOrigin)
                 + " 個のメッシュがジオメトリの中心を原点から大きく離して持っている。"
                   "LOD 選択とフォーカスが本体ではなく原点基準にズレる既知の形");

    r.Add(0, "描画リスト " + std::to_string(app.GetDrawItems().size())
             + " 件（ブロードフェーズ候補の上限は " + std::to_string(kPickCandidateLimitRef) + " 件）");

    return r;
}

// ===================== 自動 GPU インスタンシング =====================

DeepDiagReport DeepDiag::Instancing(Application& app)
{
    DeepDiagReport r;
    r.title = "自動インスタンシング";

    ++r.checked;
    const bool enabled = (ReadPersistSetting("render_instancing", 1.0) != 0.0);
    if (enabled)
        r.Add(0, "settings.json の \"render_instancing\" = 1（自動インスタンシング ON）");
    else
        r.Add(1, "settings.json の \"render_instancing\" が 0（自動インスタンシング OFF）。"
                 "A/B 計測用の設定が残っていないか確認すること");

    const std::vector<DrawItem>& items = app.GetDrawItems();
    if (items.empty())
    {
        r.skipped = "描画リストが空（まだ 1 フレームも描画していないか、描くものが無い）";
        r.Add(0, "描画リストが空なので適格率は測れなかった");
        return r;
    }

    // 不適格の理由。BuildDrawList() の適格条件と同じ順で分類し、先に当たった 1 つだけ数える。
    struct Reason { const char* label; int count; };
    Reason reasons[] = {
        { "スキンメッシュ（ボーン行列が個別）",                0 },
        { "ノードアニメ付き（行列が毎フレーム変わる）",        0 },
        { "カスタムシェーダー割当（既定 PSO に乗らない）",     0 },
        { "サブメッシュが複数（1 メッシュのものだけ畳める）",  0 },
        { "メッシュが未ロード",                                0 },
        { "マテリアルアセット(.dxmat)割当",                    0 },
        { "テクスチャ上書き（アルベド/法線/MR）",              0 },
        { "連番アニメ（animFrames > 0）",                      0 },
        { "UV スクロール",                                     0 },
        { "分類不能（適格条件の変更漏れの疑い）",              0 },
    };

    // 描画リストは「直近に構築されたもの」で、フレーム境界で呼ばれると 1 フレーム前の
    // 内容が残っていることがある。消えたエンティティの renderer ポインタは死んでいるので触らない。
    Scene*          scene = app.GetScene();
    entt::registry* reg   = scene ? &scene->GetRegistry() : nullptr;

    std::map<std::uint64_t, int> groups;
    int eligible = 0;
    int stale    = 0;
    for (const DrawItem& item : items)
    {
        if (reg != nullptr && !reg->valid(item.e)) { ++stale; continue; }
        ++r.checked;
        if (item.batchKey != 0)
        {
            ++eligible;
            ++groups[item.batchKey];
            continue;
        }

        const MeshRenderer* mr = item.renderer;
        int idx = 9;
        if      (item.skin != nullptr)                            idx = 0;
        else if (item.hasNodeAnim)                                idx = 1;
        else if (mr == nullptr)                                   idx = 4;
        else if (item.sortKey != 0u || !mr->shaderPath.empty())   idx = 2;
        else if (mr->meshes.size() != 1u)                         idx = 3;
        else if (mr->meshes[0] == nullptr)                        idx = 4;
        else if (mr->HasMaterialAsset(0u))                        idx = 5;
        else if (mr->HasAnyTextureOverride(0u))                   idx = 6;
        else if (mr->animFrames != 0)                             idx = 7;
        else if (mr->uvScrollU != 0.0f || mr->uvScrollV != 0.0f)  idx = 8;
        ++reasons[idx].count;
    }

    int batchedGroups = 0, savedDraws = 0;
    for (const auto& kv : groups)
        if (kv.second >= 2) { ++batchedGroups; savedDraws += kv.second - 1; }

    const int total = static_cast<int>(items.size()) - stale;
    if (stale > 0)
        r.skipped = "描画リストのうち " + std::to_string(stale)
                  + " 件は既に消えたエンティティのため除外した（1 フレーム前のリスト）";
    if (total <= 0)
    {
        r.Add(0, "有効な描画エントリが無いので適格率は測れなかった");
        return r;
    }

    const int pct = (total > 0) ? (eligible * 100 / total) : 0;
    r.Add(0, "描画 " + std::to_string(total) + " 件中 " + std::to_string(eligible) + " 件が適格（"
             + std::to_string(pct) + "%）。実際に畳めるのは " + std::to_string(batchedGroups)
             + " グループで、1 パスあたり最大 " + std::to_string(savedDraws) + " 件の描画コールが減る");

    if (!enabled && savedDraws > 0)
        r.Add(1, "OFF のままだと、この " + std::to_string(savedDraws) + " 件ぶんの削減が丸ごと効いていない");

    // 支配的な理由から順に出す（性能を詰めるとき、ここの 1 位を潰すのが一番効く）
    std::vector<const Reason*> ranked;
    for (const Reason& reason : reasons)
        if (reason.count > 0) ranked.push_back(&reason);
    std::sort(ranked.begin(), ranked.end(),
              [](const Reason* a, const Reason* b) { return a->count > b->count; });

    int rank = 0;
    for (const Reason* reason : ranked)
    {
        if (++rank > 5) break;
        const int p = (total > 0) ? (reason->count * 100 / total) : 0;
        r.Add(0, "不適格 " + std::to_string(rank) + " 位: " + reason->label + " … "
                 + std::to_string(reason->count) + " 件 (" + std::to_string(p) + "%)");
    }
    if (reasons[9].count > 0)
        r.Add(1, "適格条件のどれにも当てはまらない不適格ドローが " + std::to_string(reasons[9].count)
                 + " 件ある。BuildDrawList() の条件が変わってこの診断が追いついていない可能性");

    if (total >= 200 && eligible * 2 < total)
        r.Add(1, "描画 " + std::to_string(total)
                 + " 件のうち適格が半分未満。上に出ている理由の 1 位を潰すと描画コールがまとまって減る");

    return r;
}

// ===================== Lua スクリプト =====================

DeepDiagReport DeepDiag::Scripts()
{
    DeepDiagReport r;
    r.title = "Lua スクリプト";

    const fs::path assetsRoot(PathResolver::AssetsDir());
    const fs::path roots[2] = { assetsRoot, fs::path(PathResolver::ScriptsDir()) };

    // scripts/ が assets/ の中にある構成もあるので、実体パスで重複を落とす
    std::vector<fs::path> files;
    std::set<std::string> seen;
    for (const fs::path& root : roots)
        for (const fs::path& p : CollectFiles(root, {".lua"}))
        {
            std::error_code ec;
            const fs::path  canon = fs::weakly_canonical(p, ec);
            if (seen.insert(ToUtf8Lower(ec ? p : canon)).second) files.push_back(p);
        }

    if (files.empty())
    {
        r.Add(0, "検査対象の .lua が見つからない（assets / scripts のどちらにも無い）");
        return r;
    }

    const int totalFiles = static_cast<int>(files.size());
    if (totalFiles > kMaxScripts)
    {
        files.resize(static_cast<size_t>(kMaxScripts));
        r.skipped = "全 " + std::to_string(totalFiles) + " 本のうち先頭 "
                  + std::to_string(kMaxScripts) + " 本だけ検査した";
    }

    for (const fs::path& p : files)
    {
        ++r.checked;
        const std::string rel = RelDisplay(p, assetsRoot);

        std::string src;
        if (!ReadWholeFile(p, src, 4u * 1024u * 1024u))
        {
            r.Add(1, rel + " が読めない（または 4MB 超）。構文検査をスキップした");
            continue;
        }
        if (src.empty())
        {
            r.Add(1, rel + " が空ファイル");
            continue;
        }
        if (src.find('\0') != std::string::npos)
        {
            r.Add(2, rel + " にヌル文字が含まれている（テキストではない＝壊れている）");
            continue;
        }
        if (src.size() >= 3 && static_cast<int>(static_cast<unsigned char>(src[0])) == 0xEF
            && static_cast<int>(static_cast<unsigned char>(src[1])) == 0xBB
            && static_cast<int>(static_cast<unsigned char>(src[2])) == 0xBF)
            r.Add(1, rel + " の先頭に UTF-8 BOM がある。文字列から読み込む経路では構文エラーになる"
                           "（BOM 無しで保存し直すこと）");
        else if (src[0] == '#')
            r.Add(2, rel + " が '#' で始まっている（shebang）。文字列から読み込む経路では構文エラーになる");

        const LuaScanResult sc = ScanLuaSource(src);
        if (!sc.ok)
            r.Add(2, rel + (sc.line > 0 ? (" の " + std::to_string(sc.line) + " 行目付近") : std::string())
                     + ": " + sc.what);
    }

    return r;
}

// ===================== 機械可読な一括実行（MCP 用）=====================

namespace
{

nlohmann::json ReportToJson(const char* id, const DeepDiagReport& rep)
{
    nlohmann::json issues = nlohmann::json::array();
    for (const DeepDiagIssue& i : rep.issues)
    {
        nlohmann::json ji;
        ji["level"] = i.level;
        ji["text"]  = i.text;
        issues.push_back(std::move(ji));
    }

    nlohmann::json out;
    out["id"]       = id;
    out["title"]    = rep.title;
    out["checked"]  = rep.checked;
    out["errors"]   = rep.Errors();
    out["warnings"] = rep.Warnings();
    out["infos"]    = rep.Count(0);
    out["issues"]   = std::move(issues);
    out["omitted"]  = rep.omitted;
    out["skipped"]  = rep.skipped;
    return out;
}

} // namespace

std::vector<std::string> DeepDiag::AllCheckIds()
{
    return { "shaders", "textures", "models", "gamma", "scene_assets",
             "lighting", "terrain", "picking", "instancing", "scripts" };
}

nlohmann::json DeepDiag::RunAll(Application& app, const std::string& only)
{
    const std::vector<std::string> ids = DeepDiag::AllCheckIds();

    // only をカンマ区切りで分解。未知の ID は無視して summary.unknownIds へ残す
    // （MCP 側の打ち間違いが「黙って全部走る」に化けないようにするため）。
    std::set<std::string>    want;
    std::vector<std::string> unknown;
    for (size_t start = 0; start <= only.size(); )
    {
        const size_t comma = only.find(',', start);
        std::string  tok   = only.substr(start, (comma == std::string::npos)
                                                ? std::string::npos : comma - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front())) != 0)
            tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())) != 0)
            tok.pop_back();
        if (!tok.empty())
        {
            if (std::find(ids.begin(), ids.end(), tok) == ids.end()) unknown.push_back(tok);
            else                                                     want.insert(tok);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    const bool all = want.empty() && unknown.empty();

    nlohmann::json checks = nlohmann::json::array();
    int totErr = 0, totWarn = 0, totInfo = 0, ran = 0;

    auto add = [&](const char* id, const DeepDiagReport& rep)
    {
        checks.push_back(ReportToJson(id, rep));
        totErr  += rep.Errors();
        totWarn += rep.Warnings();
        totInfo += rep.Count(0);
        ++ran;
    };
    auto pick = [&](const char* id) { return all || want.count(id) != 0; };

    if (pick("shaders"))      add("shaders",      DeepDiag::Shaders());
    if (pick("textures"))     add("textures",     DeepDiag::Textures());
    if (pick("models"))       add("models",       DeepDiag::Models());
    if (pick("gamma"))        add("gamma",        DeepDiag::Gamma(app));
    if (pick("scene_assets")) add("scene_assets", DeepDiag::SceneAssets(app));
    if (pick("lighting"))     add("lighting",     DeepDiag::Lighting(app));
    if (pick("terrain"))      add("terrain",      DeepDiag::Terrain(app));
    if (pick("picking"))      add("picking",      DeepDiag::Picking(app));
    if (pick("instancing"))   add("instancing",   DeepDiag::Instancing(app));
    if (pick("scripts"))      add("scripts",      DeepDiag::Scripts());

    nlohmann::json summary;
    summary["checks"]     = ran;
    summary["errors"]     = totErr;
    summary["warnings"]   = totWarn;
    summary["infos"]      = totInfo;
    summary["ok"]         = (totErr == 0);
    summary["unknownIds"] = unknown;

    nlohmann::json out;
    out["version"] = 1;
    out["engine"]  = std::string(kEngineVersion);
    out["checks"]  = std::move(checks);
    out["summary"] = std::move(summary);
    return out;
}

} // namespace dx12e
