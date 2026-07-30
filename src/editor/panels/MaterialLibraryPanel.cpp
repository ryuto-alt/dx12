#include "editor/panels/MaterialLibraryPanel.h"
#include "core/CrashHandler.h"
#include "editor/EditorContext.h"
#include "resource/ResourceManager.h"
#include "resource/MaterialAssetManager.h"
#include "resource/MaterialAssetIO.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "core/Http.h"
#include "core/PathResolver.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace dx12e
{

namespace
{
// テンプレートタブに出す厳選候補。Poly Haven API(/assets?type=textures)を実際に取得して
// download_count上位から選定した実在ID(2026-07時点で確認済み)。カタログに無くなった場合は
// 自然に非表示になる(自己修復、DrawGrid呼び出し元のフィルタ参照)。
struct TemplateEntry { const char* id; const char* labelJa; };
constexpr TemplateEntry kTemplates[] = {
    // 地面/自然
    {"forest_ground_04",     "地面(森の下草)"},
    {"forrest_ground_01",    "地面(森)"},
    {"brown_mud_leaves_01",  "地面(土+落ち葉)"},
    {"brown_mud_dry",        "乾いた泥"},
    {"rocks_ground_02",      "岩混じりの地面"},
    {"aerial_beach_01",      "砂浜"},
    {"coast_sand_rocks_02",  "砂岩の海岸"},
    {"snow_02",              "雪"},
    {"snow_field_aerial",    "雪原"},
    {"aerial_rocks_02",      "岩肌"},
    {"rock_face_03",         "岩壁"},
    {"aerial_grass_rock",    "草+岩"},
    // 壁/レンガ/石/コンクリート
    {"beige_wall_001",       "漆喰壁(ベージュ)"},
    {"painted_plaster_wall", "塗装漆喰壁"},
    {"white_plaster_02",     "白漆喰"},
    {"red_brick",            "レンガ壁(赤)"},
    {"red_brick_03",         "レンガ壁(赤・汚れ)"},
    {"medieval_blocks_03",   "中世石ブロック壁"},
    {"concrete_floor_worn_001", "使い古したコンクリ床"},
    {"concrete_floor_02",    "コンクリ床"},
    {"cracked_concrete_wall","ひび割れコンクリ壁"},
    {"rustic_stone_wall_02", "素朴な石壁"},
    {"stone_tiles_02",       "石タイル"},
    // 木材
    {"plywood",              "合板"},
    {"wood_table_001",       "木材(テーブル)"},
    {"oak_veneer_01",        "オーク材"},
    {"weathered_brown_planks","古びた木板"},
    {"bark_brown_02",        "木の樹皮"},
    // 金属
    {"metal_plate",          "金属板"},
    {"green_metal_rust",     "サビた金属(緑)"},
    {"rust_coarse_01",       "粗いサビ"},
    {"corrugated_iron",      "トタン屋根"},
    {"roof_slates_03",       "スレート屋根"},
    // 布/革
    {"fabric_pattern_07",    "布地"},
    {"brown_leather",        "革(茶)"},
    // 床/タイル
    {"floor_tiles_06",       "床タイル"},
    {"granite_tile",         "御影石タイル"},
    {"marble_01",            "大理石"},
    {"cobblestone_floor_08", "石畳"},
    {"large_floor_tiles_02", "大判床タイル"},
    // その他
    {"asphalt_02",           "アスファルト"},
    {"gravel_embedded_concrete", "砂利入りコンクリ"},
    {"dirty_carpet",         "汚れたカーペット"},
    {"decrepit_wallpaper",   "古い壁紙"},
};

std::string LowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TodayDate()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return buf;
}

// Poly Haven /files/<id> JSON から mapType(diff/nor_gl/arm 等)・解像度・拡張子で URL を引く。
// マップ種別キーはAPI側で表記が揺れる(旧: "diff" → 現: "Diffuse" 等)ため大文字小文字を無視して照合する。
std::string PickUrl(const nlohmann::json& filesJson, const std::string& mapType,
                    const std::string& res, const std::string& fmt)
{
    const std::string wanted = LowerAscii(mapType);
    for (auto it = filesJson.begin(); it != filesJson.end(); ++it)
    {
        if (LowerAscii(it.key()) != wanted) continue;
        const auto& byRes = it.value();
        if (!byRes.is_object() || !byRes.contains(res)) return "";
        const auto& byFmt = byRes[res];
        if (!byFmt.is_object() || !byFmt.contains(fmt)) return "";
        return byFmt[fmt].value("url", "");
    }
    return "";
}

// asset.ps1(スキル)と同じ表形式でマニフェストへ追記する(CC-BY対策の出所記録。Poly HavenはCC0なので
// 帰属は"不要"固定)。ダウンロードジョブのワーカースレッドから呼ばれるため排他が要る。
void AppendManifest(const std::string& assetsDir, const std::vector<std::string>& relFiles,
                    const std::string& desc)
{
    static std::mutex s_manifestMutex;
    std::scoped_lock lk(s_manifestMutex);

    fs::path mf = fs::path(assetsDir) / "ASSET_MANIFEST.md";
    if (!fs::exists(mf))
    {
        std::ofstream init(mf, std::ios::binary | std::ios::trunc);
        init << "# Asset Manifest\n\n"
             << "asset コマンド/マテリアルライブラリで取得・生成したアセットの出所とライセンス記録。\n"
             << "CC-BY 等の帰属が必要なものは配布時にクレジットへ転記すること。\n\n"
             << "| \xe6\x97\xa5\xe4\xbb\x98 | \xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab | "
                "\xe5\x86\x85\xe5\xae\xb9 | \xe5\x87\xba\xe6\x89\x80 | \xe3\x83\xa9\xe3\x82\xa4\xe3\x82\xbb\xe3\x83\xb3\xe3\x82\xb9 | "
                "\xe5\xb8\xb0\xe5\xb1\x9e |\n"   // 日付 | ファイル | 内容 | 出所 | ライセンス | 帰属 |
             << "|---|---|---|---|---|---|\n";
    }
    std::ofstream ofs(mf, std::ios::binary | std::ios::app);
    if (!ofs) return;
    const std::string date = TodayDate();
    for (const auto& rel : relFiles)
        ofs << "| " << date << " | " << rel << " | " << desc << " | Poly Haven | CC0 | \xe4\xb8\x8d\xe8\xa6\x81 |\n";  // 不要
}

// http::Get はワイド文字列URLを取る。Poly HavenのURL/idはASCIIのみなのでUTF8変換で十分。
bool HttpGetUtf8(const std::string& url, std::vector<uint8_t>& out, const std::atomic<bool>* cancel = nullptr)
{
    return http::Get(PathResolver::Utf8ToWide(url), out, nullptr, cancel);
}
} // namespace

void MaterialLibraryPanel::Initialize(ResourceManager* resourceManager, DescriptorHeap* srvHeap,
                                      MaterialAssetManager* materialAssetManager)
{
    m_resourceManager = resourceManager;
    m_srvHeap = srvHeap;
    m_materialAssetManager = materialAssetManager;
}

MaterialLibraryPanel::~MaterialLibraryPanel()
{
    if (m_catalogThread.joinable())
        m_catalogThread.join();
    for (auto& t : m_thumbInFlight)
        if (t->worker.joinable()) t->worker.join();
    for (auto& j : m_jobs)
    {
        j->cancel.store(true);
        if (j->worker.joinable()) j->worker.join();
    }
}

void MaterialLibraryPanel::EnsureCatalogRequested(const std::string& /*assetsDir*/)
{
    if (m_catalogState.load() != CatalogState::Idle) return;
    m_catalogState.store(CatalogState::Fetching);

    m_catalogThread = std::thread([this]()
    {
        // ★ネットワーク由来の JSON を再帰パースするので、深く入れ子になった応答で
        //   スタックオーバーフローしうる。予備スタックが無いとダンプも残らない。
        CrashHandler::PrepareThread();
        std::vector<uint8_t> bytes;
        bool ok = HttpGetUtf8("https://api.polyhaven.com/assets?type=textures", bytes);
        std::vector<CatalogItem> parsed;
        if (ok)
        {
            try
            {
                auto j = nlohmann::json::parse(bytes.begin(), bytes.end());
                parsed.reserve(j.size());
                for (auto it = j.begin(); it != j.end(); ++it)
                {
                    if (!it.value().is_object()) continue;
                    CatalogItem item;
                    item.id = it.key();
                    const auto& v = it.value();
                    item.name = v.value("name", item.id);
                    if (v.contains("tags") && v["tags"].is_array())
                        for (auto& t : v["tags"]) if (t.is_string()) item.tags.push_back(t.get<std::string>());
                    if (v.contains("categories") && v["categories"].is_array())
                        for (auto& c : v["categories"]) if (c.is_string()) item.categories.push_back(c.get<std::string>());
                    parsed.push_back(std::move(item));
                }
                std::sort(parsed.begin(), parsed.end(), [](const CatalogItem& a, const CatalogItem& b) { return a.id < b.id; });
            }
            catch (const std::exception& e)
            {
                Logger::Warn("Poly Haven カタログの解析に失敗しました: {}", e.what());
                ok = false;
            }
        }
        {
            std::scoped_lock lk(m_catalogMutex);
            m_catalog = std::move(parsed);
        }
        m_catalogState.store(ok ? CatalogState::Ready : CatalogState::Failed);
    });
}

void MaterialLibraryPanel::RequestThumbnail(const std::string& id)
{
    if (m_thumbGpuHandle.count(id)) return;                                    // 取得済み(成否問わず)
    if (std::find(m_thumbRequested.begin(), m_thumbRequested.end(), id) != m_thumbRequested.end())
        return;                                                                 // 既に要求中
    if (m_thumbInFlight.size() >= kMaxConcurrentThumbFetch) return;             // 同時実行数の上限(枠が空くまで毎フレーム再試行)

    m_thumbRequested.push_back(id);
    auto fetch = std::make_unique<ThumbFetch>();
    fetch->id = id;
    ThumbFetch* raw = fetch.get();
    std::string url = "https://cdn.polyhaven.com/asset_img/thumbs/" + id + ".png?width=128&height=128";
    fetch->worker = std::thread([raw, url]()
    {
        CrashHandler::PrepareThread();
        HttpGetUtf8(url, raw->bytes);
        raw->done.store(true);
    });
    m_thumbInFlight.push_back(std::move(fetch));
}

void MaterialLibraryPanel::LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    // ★ダウンロードジョブのスレッドを回収する。サムネイル側（下）は done を見て
    //   join + erase しているのに、ジョブ側だけ**デストラクタまで一度も join されず**、
    //   1 マテリアル落とすごとにスレッドハンドルとスタック予約が残っていた。
    //   さらに m_jobs はグリッド描画で毎フレーム 2 回線形走査されるので、
    //   落とすほど窓が重くなる。
    //   失敗ジョブだけはエラー文言を出すために残す（件数は高が知れている）。
    for (size_t i = 0; i < m_jobs.size(); )
    {
        DownloadJob* j = m_jobs[i].get();
        if (!j->done.load()) { ++i; continue; }
        if (j->worker.joinable()) j->worker.join();   // done 後の join は即返る
        if (j->phase.load() == -1) { ++i; continue; } // 失敗は表示用に残す
        m_jobs.erase(m_jobs.begin() + static_cast<ptrdiff_t>(i));
    }

    if (!cmdList || !m_resourceManager) return;

    for (size_t i = 0; i < m_thumbInFlight.size(); )
    {
        ThumbFetch* f = m_thumbInFlight[i].get();
        if (!f->done.load()) { ++i; continue; }

        if (f->worker.joinable()) f->worker.join();  // done==true 後の join は即返る(同期の確定点)

        u64 gpuHandle = 0;
        if (!f->bytes.empty())
        {
            Texture* tex = m_resourceManager->GetOrLoadEmbeddedTexture(
                "polyhaven_thumb_" + f->id, f->bytes.data(), f->bytes.size(), "png", cmdList);
            if (tex && tex->GetSrvIndex() != UINT32_MAX)
                gpuHandle = m_srvHeap->GetGpuHandle(tex->GetSrvIndex()).ptr;
        }
        m_thumbGpuHandle[f->id] = gpuHandle;  // 0 = 失敗(以後は種別アイコン相当の空表示のまま)

        m_thumbInFlight.erase(m_thumbInFlight.begin() + static_cast<ptrdiff_t>(i));
    }
}

bool MaterialLibraryPanel::IsDownloaded(const std::string& id, const std::string& assetsDir) const
{
    std::error_code ec;
    return fs::exists(fs::path(assetsDir) / "materials" / (id + ".dxmat"), ec);
}

void MaterialLibraryPanel::StartDownload(const std::string& id, const std::string& assetsDir)
{
    for (auto& j : m_jobs)
        if (j->id == id && !j->done.load()) return;  // 二重起動防止

    auto job = std::make_unique<DownloadJob>();
    job->id = id;
    job->resolution = (m_resolutionIndex == 0) ? "1k" : (m_resolutionIndex == 2) ? "4k" : "2k";
    DownloadJob* raw = job.get();
    const std::string res = job->resolution;

    raw->worker = std::thread([raw, id, res, assetsDir]()
    {
        CrashHandler::PrepareThread();   // ここも応答 JSON を再帰パースする
        auto fail = [&](const std::string& msg)
        {
            raw->error = msg;
            raw->phase.store(-1);
            raw->done.store(true);
        };

        raw->phase.store(0);
        std::vector<uint8_t> filesBytes;
        if (!HttpGetUtf8("https://api.polyhaven.com/files/" + id, filesBytes, &raw->cancel))
        { fail("files APIの取得に失敗"); return; }

        nlohmann::json filesJson;
        try { filesJson = nlohmann::json::parse(filesBytes.begin(), filesBytes.end()); }
        catch (const std::exception& e) { fail(std::string("files JSON解析失敗: ") + e.what()); return; }

        // diff(jpgで容量節約) / nor_gl(pngのみ。nor_dxは使わない=シェーダーがOpenGL規約のため) / arm(png、AO+Rough+Metal)
        // albedoのキー名はAPI側で "diff" → "Diffuse" に変わった実績があるため両方試す。
        std::string diffUrl = PickUrl(filesJson, "diff", res, "jpg");
        if (diffUrl.empty()) diffUrl = PickUrl(filesJson, "diffuse", res, "jpg");
        std::string normUrl = PickUrl(filesJson, "nor_gl", res, "png");
        std::string armUrl  = PickUrl(filesJson, "arm", res, "png");
        if (diffUrl.empty())
        {
            std::string keys;
            for (auto it = filesJson.begin(); it != filesJson.end(); ++it)
                keys += (keys.empty() ? "" : ", ") + it.key();
            fail("albedo(diff)テクスチャが見つかりません (APIのキー: " + keys + ")");
            return;
        }

        fs::path outDir = fs::path(assetsDir) / "textures" / id;
        std::error_code ec;
        fs::create_directories(outDir, ec);

        auto downloadOne = [&](const std::string& url, const std::string& ext, std::string& outRelPath) -> bool
        {
            if (url.empty() || raw->cancel.load()) return false;
            std::vector<uint8_t> bytes;
            if (!HttpGetUtf8(url, bytes, &raw->cancel) || bytes.empty()) return false;
            std::string fname = id + "_" + ext;
            fs::path finalPath = outDir / fname;
            fs::path partPath  = outDir / (fname + ".part");
            {
                std::ofstream ofs(partPath, std::ios::binary | std::ios::trunc);
                if (!ofs) return false;
                ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            std::error_code renEc;
            fs::rename(partPath, finalPath, renEc);
            if (renEc) { fs::remove(partPath, ec); return false; }
            outRelPath = "textures/" + id + "/" + fname;
            return true;
        };

        raw->phase.store(1);
        std::vector<std::string> savedRel;
        MaterialAssetData data;
        data.name = id;
        if (!downloadOne(diffUrl, "diff.jpg", data.albedoPath)) { fail("albedoの保存に失敗"); return; }
        savedRel.push_back(data.albedoPath);
        if (downloadOne(normUrl, "nor_gl.png", data.normalPath)) savedRel.push_back(data.normalPath);
        if (downloadOne(armUrl, "arm.png", data.metalRoughnessPath)) savedRel.push_back(data.metalRoughnessPath);

        data.metallic = 1.0f; data.roughness = 1.0f;
        data.uvTilingU = 1.0f; data.uvTilingV = 1.0f;
        data.source = "Poly Haven"; data.license = "CC0";

        raw->phase.store(2);
        fs::path matDir = fs::path(assetsDir) / "materials";
        fs::create_directories(matDir, ec);
        fs::path matPath = matDir / (id + ".dxmat");
        {
            std::ofstream ofs(matPath, std::ios::binary | std::ios::trunc);
            if (!ofs) { fail("マテリアルアセットの書き込みに失敗"); return; }
            std::string json = SerializeMaterialAsset(data);
            ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
        }
        savedRel.push_back("materials/" + id + ".dxmat");

        AppendManifest(assetsDir, savedRel, "texture: " + id + " (Poly Haven Material Library, " + res + ")");

        raw->phase.store(3);
        raw->done.store(true);
    });

    m_jobs.push_back(std::move(job));
}

void MaterialLibraryPanel::DrawGrid(EditorContext& /*ctx*/, const std::string& assetsDir,
                                    const std::vector<const CatalogItem*>& items)
{
    constexpr float kCellSize = 144.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    int columns = (std::max)(1, static_cast<int>(avail / (kCellSize + 12.0f)));

    if (ImGui::BeginTable("MatLibGrid", columns))
    {
        for (const CatalogItem* item : items)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(item->id.c_str());
            ImGui::BeginGroup();

            auto handleIt = m_thumbGpuHandle.find(item->id);
            if (handleIt == m_thumbGpuHandle.end())
                RequestThumbnail(item->id);

            if (handleIt != m_thumbGpuHandle.end() && handleIt->second != 0)
            {
                ImGui::Image(static_cast<ImTextureID>(handleIt->second), ImVec2(kCellSize, kCellSize));
            }
            else
            {
                ImGui::Dummy(ImVec2(kCellSize, kCellSize));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                dl->AddRectFilled(mn, mx, IM_COL32(38, 40, 46, 255), 4.0f);
            }

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kCellSize);
            ImGui::TextWrapped("%s", item->name.c_str());
            ImGui::PopTextWrapPos();

            bool downloaded = IsDownloaded(item->id, assetsDir);
            const DownloadJob* activeJob = nullptr;
            for (auto& j : m_jobs)
                if (j->id == item->id && !j->done.load()) { activeJob = j.get(); break; }

            if (activeJob)
            {
                const char* phaseLabel[] = { "\xe5\x8f\x96\xe5\xbe\x97\xe4\xb8\xad...", "\xe4\xbf\x9d\xe5\xad\x98\xe4\xb8\xad...",
                                             "\xe7\x99\xbb\xe9\x8c\xb2\xe4\xb8\xad...", "\xe5\xae\x8c\xe4\xba\x86" };  // 取得中/保存中/登録中/完了
                int ph = (std::max)(0, activeJob->phase.load());
                ImGui::TextDisabled("%s", phaseLabel[(std::min)(ph, 3)]);
            }
            else if (downloaded)
            {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "\xe2\x9c\x93 \xe5\x8f\x96\xe5\xbe\x97\xe6\xb8\x88\xe3\x81\xbf");  // ✓ 取得済み
            }
            else
            {
                if (ImGui::Button("\xe3\x83\x80\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\xad\xe3\x83\xbc\xe3\x83\x89", ImVec2(kCellSize, 0)))  // ダウンロード
                    StartDownload(item->id, assetsDir);
            }

            // 直近で終了したジョブのエラー表示(一度だけ状態確認。エラー文字列は done==true 後のみ有効)
            for (auto& j : m_jobs)
            {
                if (j->id == item->id && j->done.load() && j->phase.load() == -1)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", j->error.c_str());
                    break;
                }
            }

            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void MaterialLibraryPanel::RenderWindow(EditorContext& ctx, const std::string& assetsDir)
{
    if (!ctx.showMaterialLibrary) return;

    if (m_catalogThread.joinable() && m_catalogState.load() != CatalogState::Fetching)
        m_catalogThread.join();  // Ready/Failed に遷移済み = ワーカーは終了済みなので即返る

    EnsureCatalogRequested(assetsDir);

    ImGui::SetNextWindowSize(ImVec2(920.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe3\x83\xa9\xe3\x83\xaa "
                      "(Poly Haven)###MaterialLibraryFloating",  // マテリアルライブラリ (Poly Haven)
                      &ctx.showMaterialLibrary, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    // 3Dビューポートの上に重なって浮かぶことがあるので、ホバー中はシーンカメラのドラッグ/ホイール
    // 操作が同時に反応しないよう EditorContext 経由で伝える(スクロール等がカメラズームに漏れる対策)。
    // ThisFrame に書き、読む側は前フレームの確定値を見る。RootAndChildWindows でグリッドの
    // BeginChild スクロール領域上のホバーも拾う。
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    const CatalogState state = m_catalogState.load();
    if (state == CatalogState::Fetching)
    {
        ImGui::TextDisabled("\xe3\x82\xab\xe3\x82\xbf\xe3\x83\xad\xe3\x82\xb0\xe3\x82\x92\xe5\x8f\x96\xe5\xbe\x97\xe4\xb8\xad...");  // カタログを取得中...
        ImGui::End();
        return;
    }
    if (state == CatalogState::Failed)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
            "\xe3\x82\xaa\xe3\x83\x95\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\xb3\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82"  // オフラインです。
            "\xe3\x83\x8d\xe3\x83\x83\xe3\x83\x88\xe3\x83\xaf\xe3\x83\xbc\xe3\x82\xaf\xe6\x8e\xa5\xe7\xb6\x9a\xe3\x82\x92\xe7\xa2\xba\xe8\xaa\x8d\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xe3\x80\x82");
        ImGui::SameLine();
        if (ImGui::Button("\xe5\x86\x8d\xe8\xa9\xa6\xe8\xa1\x8c Retry"))
            m_catalogState.store(CatalogState::Idle);
        ImGui::End();
        return;
    }

    // ---- タブ ----
    if (ImGui::BeginTabBar("MatLibTabs"))
    {
        if (ImGui::BeginTabItem("\xe3\x83\x86\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xac\xe3\x83\xbc\xe3\x83\x88"))  // テンプレート
        {
            m_templatesTab = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("\xe5\x85\xa8\xe4\xbd\x93 All"))
        {
            m_templatesTab = false;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##Search", "\xe6\xa4\x9c\xe7\xb4\xa2 (\xe8\x8b\xb1\xe5\x8d\x98\xe8\xaa\x9e)", m_searchBuf, sizeof(m_searchBuf));  // 検索 (英単語)
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    const char* resLabels[] = { "1k", "2k", "4k" };
    ImGui::Combo("\xe8\xa7\xa3\xe5\x83\x8f\xe5\xba\xa6", &m_resolutionIndex, resLabels, 3);  // 解像度

    if (!m_templatesTab)
    {
        static const char* kCats[] = { "", "ground", "wall", "brick", "stone", "wood", "metal", "roof", "tiles" };
        for (const char* cat : kCats)
        {
            ImGui::SameLine();
            bool active = (m_activeCategory == cat);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            if (ImGui::SmallButton(cat[0] ? cat : "All")) m_activeCategory = cat;
            if (active) ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("##MatLibScroll");

    if (m_templatesTab)
    {
        std::vector<const CatalogItem*> items;
        for (const auto& tpl : kTemplates)
        {
            auto it = std::find_if(m_catalog.begin(), m_catalog.end(),
                [&](const CatalogItem& c) { return c.id == tpl.id; });
            if (it != m_catalog.end()) items.push_back(&*it);
        }
        DrawGrid(ctx, assetsDir, items);
    }
    else
    {
        std::string query = LowerAscii(m_searchBuf);
        std::vector<const CatalogItem*> items;
        for (const auto& c : m_catalog)
        {
            if (!m_activeCategory.empty() &&
                std::find(c.categories.begin(), c.categories.end(), m_activeCategory) == c.categories.end())
                continue;
            if (!query.empty())
            {
                std::string hay = LowerAscii(c.id + " " + c.name);
                for (auto& t : c.tags) hay += " " + LowerAscii(t);
                if (hay.find(query) == std::string::npos) continue;
            }
            items.push_back(&c);
            if (items.size() >= 200) break;  // 1回のグリッド描画に対する簡易上限(全件は検索/カテゴリで絞る想定)
        }
        if (items.size() >= 200)
            ImGui::TextDisabled("\xe8\xa1\xa8\xe7\xa4\xba\xe4\xbb\xb6\xe6\x95\xb0\xe3\x81\x8c" "200" "\xe4\xbb\xb6\xe3\x81\xab\xe5\x88\xb6\xe9\x99\x90\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99"
                              "\xe3\x80\x82\xe3\x82\xad\xe3\x83\xbc\xe3\x83\xaf\xe3\x83\xbc\xe3\x83\x89/\xe3\x82\xab\xe3\x83\x86\xe3\x82\xb4\xe3\x83\xaa\xe3\x81\xa7\xe7\xb5\x9e\xe3\x82\x8a\xe8\xbe\xbc\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84"); // 表示件数が200件に制限されています。キーワード/カテゴリで絞り込んでください
        DrawGrid(ctx, assetsDir, items);
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace dx12e
