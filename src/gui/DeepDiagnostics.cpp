#include "gui/DeepDiagnostics.h"

#include "core/Application.h"
#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "resource/ModelLoader.h"
#include "resource/ShaderManager.h"
#include "resource/ShaderRegistry.h"
#include "resource/TextureLoader.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

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

} // namespace dx12e
