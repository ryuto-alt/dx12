#include "editor/panels/NavMeshPanel.h"

#include "core/Logger.h"
#include "editor/EditorContext.h"
#include "editor/PropertyGrid.h"
#include "nav/NavTypes.h"
#include "scene/NavSceneGather.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <cstdio>

namespace dx12e
{
namespace NavMeshPanel
{

bool BuildForScene(Scene& scene, std::string& outLog, std::string& outError)
{
    outLog.clear();
    outError.clear();

    nav::NavInputGeometry geom;
    NavGatherStats gs{};
    if (!GatherNavGeometry(scene.GetRegistry(), geom, gs))
    {
        outError = "ナビメッシュに含められるメッシュがシーンに無い"
                   "（床になるメッシュを置くか、navMeshIgnore タグを外すこと）";
        return false;
    }

    char head[256];
    std::snprintf(head, sizeof(head),
                  "0. 収集: %d エンティティ / %d メッシュ / %d 三角形"
                  "（スキン %d 個・タグ除外 %d 個をスキップ）",
                  gs.entityCount, gs.meshCount, gs.triCount, gs.skippedSkinned, gs.skippedTagged);
    outLog = std::string(head) + "\n";

    nav::NavBuildReport rep;
    nav::NavMesh built;
    const bool ok = nav::BuildNavMesh(geom, scene.GetNavConfig(), built, rep);
    outLog += rep.stageLog;
    if (!ok)
    {
        outError = rep.error;
        return false;
    }
    scene.GetNavMesh() = std::move(built);
    Logger::Info("NavMesh built: {} polys, {:.1f} m2, {:.0f} ms",
                 scene.GetNavMesh().PolyCount(),
                 scene.GetNavMesh().GetStats().walkableArea, rep.totalMs);
    return true;
}

namespace
{

struct PanelState
{
    std::string log;
    std::string error;
    bool  building = false;
    // 経路テスト
    float testFrom[3] = { 0.0f, 0.0f, 0.0f };
    float testTo[3]   = { 5.0f, 0.0f, 5.0f };
    int   testPoints  = -1;
    float testLength  = 0.0f;
    std::string testMsg;
};

void RenderParams(Scene& scene, bool& changed)
{
    nav::NavBuildConfig& c = scene.GetNavConfig();

    if (pg::Begin("##navAgent"))
    {
        // ★pg::Group のラベルは左カラム(全幅の 42%)に描かれる。長い説明を入れると
        //   途中で切れて読めなくなるので、見出しは短く・説明は各行の tip へ入れること。
        pg::Group("エージェント");
        changed |= pg::Float("半径 (m)", &c.agentRadius, 0.01f, 0.0f, 5.0f, "%.2f", nullptr,
                             "壁からこのぶん削る。実際のキャラのカプセル半径に合わせる。"
                             "大きすぎると狭い通路が全部消える");
        changed |= pg::Float("高さ (m)", &c.agentHeight, 0.01f, 0.1f, 10.0f, "%.2f", nullptr,
                             "頭上クリアランス。これより天井が低い場所は歩行不可になる");
        changed |= pg::Float("またげる段差 (m)", &c.agentMaxClimb, 0.01f, 0.0f, 5.0f, "%.2f", nullptr,
                             "階段一段ぶん。これ以下の高さ差は繋がり、超えると崖として切れる");
        changed |= pg::SliderFloat("歩ける最大傾斜 (度)", &c.agentMaxSlope, 0.0f, 89.0f, "%.0f", nullptr,
                             "★坂道の許容角。面の法線がこれより寝ていれば歩行面として採用する");
        pg::End();
    }

    if (pg::Begin("##navRes"))
    {
        pg::Group("精度");
        changed |= pg::Float("セルサイズ (m)", &c.cellSize, 0.005f, 0.02f, 2.0f, "%.3f", nullptr,
                             "水平方向のボクセル辺長。エージェント半径の 1/2〜1/3 が目安");
        changed |= pg::Float("セル高さ (m)", &c.cellHeight, 0.005f, 0.01f, 1.0f, "%.3f", nullptr,
                             "垂直方向の分解能。またげる段差の 1/3 以下にすること");
        changed |= pg::Float("輪郭の許容誤差 (vox)", &c.maxSimplificationErr, 0.05f, 0.1f, 5.0f, "%.2f", nullptr,
                             "壁沿いのギザギザをどこまで均すか。大きいほどポリゴンが減るが壁に食い込む");
        changed |= pg::Float("辺の最大長 (m)", &c.maxEdgeLen, 0.1f, 0.0f, 100.0f, "%.1f", nullptr,
                             "長い壁を分割する上限。0 で無制限");
        changed |= pg::SliderInt("1ポリの頂点数", &c.maxVertsPerPoly, 3, 12, nullptr,
                             "大きいほどポリゴンが減って A* が速い。6 が定番");
        pg::End();
    }

    if (pg::Begin("##navRegion"))
    {
        pg::Group("領域");
        changed |= pg::Float("最小領域 (m2)", &c.minRegionArea, 0.1f, 0.0f, 200.0f, "%.1f", nullptr,
                             "これより小さい孤立した島は捨てる。★箱や柱のような閉じた立体は"
                             "「底面と天面の間」に頭上クリアランスが空くと内部の床も歩行面として残る"
                             "（どこからも行けない島になる）。これを消すには此処を上げる（8〜20 が実用値）");
        changed |= pg::Float("併合する領域 (m2)", &c.mergeRegionArea, 0.5f, 0.0f, 500.0f, "%.1f", nullptr,
                             "これより小さい領域は隣へ吸収する。細切れが減って経路が素直になる");
        int mode = c.monotonePartition ? 1 : 0;
        static const char* kModes[] = { "分水嶺（既定・形が良い）", "monotone（速い・細長い）" };
        if (pg::Combo("分割方式", &mode, kModes, 2,
                      "分水嶺は距離場から領域を育てるので自然な形になる。"
                      "monotone は掃引 1 回で速いが細長い領域が出やすい"))
        { c.monotonePartition = (mode == 1); changed = true; }
        pg::End();
    }

    if (pg::Begin("##navFilter"))
    {
        pg::Group("フィルタ / 範囲");
        changed |= pg::Checkbox("崖ぎわを除外", &c.filterLedgeSpans,
                             "隣が段差を超えて落ちている縁を歩行不可にする。切ると AI が崖から落ちる");
        changed |= pg::Checkbox("低い障害物をまたぐ", &c.filterLowHanging,
                             "縁石など段差以下の障害物の上を歩けるようにする");
        changed |= pg::Checkbox("範囲を手動指定", &c.useBounds,
                             "切ると全メッシュの AABB を自動で使う。広大なシーンの一部だけ焼きたい時に入れる");
        if (c.useBounds)
        {
            changed |= pg::Float3("範囲 最小", c.boundsMin, 0.1f);
            changed |= pg::Float3("範囲 最大", c.boundsMax, 0.1f);
        }
        pg::End();
    }
}

void RenderStats(Scene& scene)
{
    const nav::NavMesh& nm = scene.GetNavMesh();
    if (pg::Begin("##navStats"))
    {
        pg::Group("結果");
        if (nm.Empty())
        {
            pg::Text("状態", "未生成");
        }
        else
        {
            const auto& st = nm.GetStats();
            pg::Text("ポリゴン", "%d", st.polyCount);
            pg::Text("頂点", "%d", st.vertCount);
            pg::Text("歩行面積", "%.1f m2", st.walkableArea);
            pg::Text("高さサンプル", "%d (%dx%d 格子)", st.sampleCount, st.gridW, st.gridH);
            // 読み込んだ .nav は生成時間を持たない（0 と出すと「一瞬で焼けた」と誤解される）
            if (st.buildMs > 0.0f) pg::Text("生成時間", "%.0f ms", st.buildMs);
            else                   pg::Text("生成時間", "-（.nav から読み込み）");
            pg::Text("メモリ", "%.2f MB", static_cast<double>(st.memoryBytes) / (1024.0 * 1024.0));
            f32 bmin[3], bmax[3];
            nm.GetBounds(bmin, bmax);
            // 1 行に 6 個並べると既定幅で右端が切れるので 2 行に分ける
            pg::Text("範囲 最小", "%.1f, %.1f, %.1f", bmin[0], bmin[1], bmin[2]);
            pg::Text("範囲 最大", "%.1f, %.1f, %.1f", bmax[0], bmax[1], bmax[2]);
        }
        pg::End();
    }
}

void RenderPathTest(Scene& scene, PanelState& st)
{
    if (!pg::Begin("##navTest")) return;
    pg::Group("経路テスト");
    pg::Float3("開始", st.testFrom, 0.1f);
    pg::Float3("目標", st.testTo, 0.1f);
    pg::Label("実行");
    if (ImGui::Button("経路を計算", ImVec2(-FLT_MIN, 0)))
    {
        st.testPoints = -1; st.testLength = 0.0f; st.testMsg.clear();
        if (scene.GetNavMesh().Empty())
        {
            st.testMsg = "先にビルドすること";
        }
        else
        {
            const f32 ext[3] = { 3.0f, 4.0f, 3.0f };
            std::vector<f32> path;
            st.testPoints = scene.GetNavMesh().FindPath(st.testFrom, st.testTo, ext, path);
            for (size_t i = 0; i + 5 < path.size(); i += 3)
            {
                const f32 dx = path[i + 3] - path[i + 0];
                const f32 dy = path[i + 4] - path[i + 1];
                const f32 dz = path[i + 5] - path[i + 2];
                st.testLength += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            if (st.testPoints <= 0) st.testMsg = "経路なし（開始/目標がナビメッシュから遠い可能性）";
        }
    }
    if (st.testPoints > 0)
        pg::Text("結果", "%d 点 / 全長 %.2f m", st.testPoints, st.testLength);
    else if (!st.testMsg.empty())
        pg::Text("結果", "%s", st.testMsg.c_str());
    pg::End();
}

} // namespace

void Render(Scene& scene, EditorContext& ctx)
{
    if (!ctx.showNavMesh) return;

    static PanelState st;

    ImGui::SetNextWindowSize(ImVec2(470, 760), ImGuiCond_FirstUseEver);
    // ### 付きの固定 ID（UI 自動テストが窓を引くのに使う。表示名を変えても壊れない）
    if (!ImGui::Begin("ナビメッシュ###NavMeshFloating", &ctx.showNavMesh))
    {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "シーンのメッシュを実際の三角形のままボクセル化して、歩ける面だけを取り出す。"
        "焼いた結果はシーンの隣の .nav に保存される（シーンを保存すると一緒に書かれる）。");
    ImGui::Separator();

    bool changed = false;
    RenderParams(scene, changed);

    ImGui::Spacing();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("ビルド", ImVec2(w * 0.48f, 30)))
    {
        st.error.clear();
        if (!BuildForScene(scene, st.log, st.error))
            Logger::Warn("ナビメッシュのビルドに失敗: {}", st.error);
    }
    ImGui::SameLine();
    if (ImGui::Button("クリア", ImVec2(-FLT_MIN, 30)))
    {
        scene.GetNavMesh().Clear();
        st.log.clear(); st.error.clear(); st.testPoints = -1; st.testMsg.clear();
    }

    bool dbg = scene.GetNavDebugDraw();
    if (ImGui::Checkbox("シーンビューにワイヤ表示", &dbg)) scene.SetNavDebugDraw(dbg);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("物理デバッグ描画と同じ線で、歩行ポリゴンの輪郭を重ねる");

    if (!st.error.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", st.error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    RenderStats(scene);
    ImGui::Spacing();
    RenderPathTest(scene, st);

    if (!st.log.empty())
    {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("生成ログ"))
        {
            ImGui::BeginChild("##navlog", ImVec2(0, 150), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(st.log.c_str());
            ImGui::EndChild();
        }
    }

    ImGui::End();
}

} // namespace NavMeshPanel
} // namespace dx12e
