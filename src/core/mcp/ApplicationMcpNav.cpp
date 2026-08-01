// ===========================================================================
// MCP: ナビメッシュ（生成 / 設定 / 経路 / レイ / 可視化）
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
//
// 生成の実体は NavMeshPanel::BuildForScene（エディタの「ビルド」ボタンと同じ関数）。
// ＝AI が焼いた結果と人が焼いた結果が必ず一致する。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;

namespace
{
// 設定を JSON へ（info / settings が同じ形を返す＝往復できる）
nlohmann::json NavConfigJson(const nav::NavBuildConfig& c)
{
    return {
        {"cellSize",             c.cellSize},
        {"cellHeight",           c.cellHeight},
        {"agentHeight",          c.agentHeight},
        {"agentRadius",          c.agentRadius},
        {"agentMaxClimb",        c.agentMaxClimb},
        {"agentMaxSlope",        c.agentMaxSlope},
        {"minRegionArea",        c.minRegionArea},
        {"mergeRegionArea",      c.mergeRegionArea},
        {"maxEdgeLen",           c.maxEdgeLen},
        {"maxSimplificationErr", c.maxSimplificationErr},
        {"maxVertsPerPoly",      c.maxVertsPerPoly},
        {"monotonePartition",    c.monotonePartition},
        {"filterLedgeSpans",     c.filterLedgeSpans},
        {"filterLowHanging",     c.filterLowHanging},
        {"useBounds",            c.useBounds},
        {"boundsMin",            {c.boundsMin[0], c.boundsMin[1], c.boundsMin[2]}},
        {"boundsMax",            {c.boundsMax[0], c.boundsMax[1], c.boundsMax[2]}},
    };
}

// params から設定を上書き（指定されたキーだけ）。戻り値 = 触ったキー数。
i32 ApplyNavConfig(const nlohmann::json& params, nav::NavBuildConfig& c)
{
    i32 n = 0;
    auto has = [&](const char* k) { auto it = params.find(k); return it != params.end() && !it->is_null(); };

    if (has("cellSize"))             { c.cellSize    = McpFloatParam(params, "cellSize", c.cellSize, 0.02f, 2.0f); ++n; }
    if (has("cellHeight"))           { c.cellHeight  = McpFloatParam(params, "cellHeight", c.cellHeight, 0.01f, 1.0f); ++n; }
    if (has("agentHeight"))          { c.agentHeight = McpFloatParam(params, "agentHeight", c.agentHeight, 0.1f, 10.0f); ++n; }
    if (has("agentRadius"))          { c.agentRadius = McpFloatParam(params, "agentRadius", c.agentRadius, 0.0f, 5.0f); ++n; }
    if (has("agentMaxClimb"))        { c.agentMaxClimb = McpFloatParam(params, "agentMaxClimb", c.agentMaxClimb, 0.0f, 5.0f); ++n; }
    if (has("agentMaxSlope"))        { c.agentMaxSlope = McpFloatParam(params, "agentMaxSlope", c.agentMaxSlope, 0.0f, 89.0f); ++n; }
    if (has("minRegionArea"))        { c.minRegionArea = McpFloatParam(params, "minRegionArea", c.minRegionArea, 0.0f, 500.0f); ++n; }
    if (has("mergeRegionArea"))      { c.mergeRegionArea = McpFloatParam(params, "mergeRegionArea", c.mergeRegionArea, 0.0f, 2000.0f); ++n; }
    if (has("maxEdgeLen"))           { c.maxEdgeLen = McpFloatParam(params, "maxEdgeLen", c.maxEdgeLen, 0.0f, 500.0f); ++n; }
    if (has("maxSimplificationErr")) { c.maxSimplificationErr = McpFloatParam(params, "maxSimplificationErr", c.maxSimplificationErr, 0.1f, 10.0f); ++n; }
    if (has("maxVertsPerPoly"))      { c.maxVertsPerPoly = McpIntParam(params, "maxVertsPerPoly", c.maxVertsPerPoly, 3, 12); ++n; }
    if (has("monotonePartition"))    { c.monotonePartition = params["monotonePartition"].get<bool>(); ++n; }
    if (has("filterLedgeSpans"))     { c.filterLedgeSpans = params["filterLedgeSpans"].get<bool>(); ++n; }
    if (has("filterLowHanging"))     { c.filterLowHanging = params["filterLowHanging"].get<bool>(); ++n; }
    if (has("useBounds"))            { c.useBounds = params["useBounds"].get<bool>(); ++n; }

    DirectX::XMFLOAT3 v{};
    if (McpTryVec3(params, "boundsMin", v)) { c.boundsMin[0] = v.x; c.boundsMin[1] = v.y; c.boundsMin[2] = v.z; c.useBounds = true; ++n; }
    if (McpTryVec3(params, "boundsMax", v)) { c.boundsMax[0] = v.x; c.boundsMax[1] = v.y; c.boundsMax[2] = v.z; c.useBounds = true; ++n; }
    return n;
}

nlohmann::json NavStatsJson(const nav::NavMesh& nm)
{
    if (nm.Empty()) return { {"built", false} };
    const auto& s = nm.GetStats();
    f32 bmin[3], bmax[3];
    nm.GetBounds(bmin, bmax);
    return {
        {"built",        true},
        {"polyCount",    s.polyCount},
        {"vertCount",    s.vertCount},
        {"sampleCount",  s.sampleCount},
        {"gridW",        s.gridW},
        {"gridH",        s.gridH},
        {"walkableArea", s.walkableArea},
        {"buildMs",      s.buildMs},
        {"memoryBytes",  static_cast<u64>(s.memoryBytes)},
        {"boundsMin",    {bmin[0], bmin[1], bmin[2]}},
        {"boundsMax",    {bmax[0], bmax[1], bmax[2]}},
    };
}

// 探索の許容ずれ。指定が無ければセルサイズ基準の妥当な既定にする。
void ReadExtents(const nlohmann::json& params, const nav::NavBuildConfig& c, f32 out[3])
{
    const f32 def = (std::max)(2.0f, c.cellSize * 8.0f);
    const f32 e = McpFloatParam(params, "searchRadius", def, 0.05f, 200.0f);
    out[0] = e;
    out[1] = McpFloatParam(params, "searchHeight", (std::max)(c.agentHeight * 2.0f, 4.0f), 0.05f, 500.0f);
    out[2] = e;
}
} // namespace

void Application::RegisterMcpNavMethods()
{
    using json = nlohmann::json;

    // ════════════════════════════════════════════════════════════
    //  生成 / 設定
    // ════════════════════════════════════════════════════════════
    McpDefine("navmesh_build",
              "agentHeight:number,agentMaxClimb:number,agentMaxSlope:number,agentRadius:number,"
              "boundsMax:vec3,boundsMin:vec3,cellHeight:number,cellSize:number,"
              "filterLedgeSpans:bool,filterLowHanging:bool,maxEdgeLen:number,"
              "maxSimplificationErr:number,maxVertsPerPoly:int,mergeRegionArea:number,"
              "minRegionArea:number,monotonePartition:bool,useBounds:bool",
              DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot build navmesh while Playing",
                               "Play 中に焼いても Stop で巻き戻る。先に dx12_stop してから焼くこと");

            const i32 touched = ApplyNavConfig(params, m_scene->GetNavConfig());

            std::string log, err;
            if (!NavMeshPanel::BuildForScene(*m_scene, log, err))
                throw McpError(McpErr::InvalidParam, err.empty() ? "navmesh build failed" : err,
                               "ログ(stageLog)に各段階の件数が出る。0 になっている段階の直前の設定が原因",
                               log.empty() ? std::vector<std::string>{} : std::vector<std::string>{ log });

            resp["ok"]       = true;
            resp["settingsChanged"] = touched;
            resp["stats"]    = NavStatsJson(m_scene->GetNavMesh());
            resp["config"]   = NavConfigJson(m_scene->GetNavConfig());
            resp["stageLog"] = log;
            resp["note"]     = "焼いた実体はシーンの隣の .nav。dx12_save_scene で書き出される";
        });

    McpDefine("navmesh_settings",
              "agentHeight:number,agentMaxClimb:number,agentMaxSlope:number,agentRadius:number,"
              "boundsMax:vec3,boundsMin:vec3,cellHeight:number,cellSize:number,"
              "filterLedgeSpans:bool,filterLowHanging:bool,maxEdgeLen:number,"
              "maxSimplificationErr:number,maxVertsPerPoly:int,mergeRegionArea:number,"
              "minRegionArea:number,monotonePartition:bool,useBounds:bool",
              DX12E_MCP_HANDLER
        {
            const i32 touched = ApplyNavConfig(params, m_scene->GetNavConfig());
            resp["applied"] = touched;
            resp["config"]  = NavConfigJson(m_scene->GetNavConfig());
            resp["note"]    = touched > 0 ? "設定だけ変えた。反映するには dx12_navmesh_build"
                                          : "引数が無いので現在値を返しただけ";
        });

    McpDefine("navmesh_info", "", DX12E_MCP_HANDLER
        {
            resp["config"] = NavConfigJson(m_scene->GetNavConfig());
            resp["stats"]  = NavStatsJson(m_scene->GetNavMesh());
            resp["debugDraw"] = m_scene->GetNavDebugDraw();
            resp["note"] = "built=false なら未生成。dx12_navmesh_build で焼く";
        });

    McpDefine("navmesh_clear", "", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot clear navmesh while Playing",
                               "先に dx12_stop で Editor へ戻すこと");
            const i32 before = m_scene->GetNavMesh().PolyCount();
            m_scene->GetNavMesh().Clear();
            resp["cleared"] = before;
            resp["note"] = "シーンを保存すると隣の .nav も消える";
        });

    McpDefine("navmesh_debug", "enabled:bool", DX12E_MCP_HANDLER
        {
            auto it = params.find("enabled");
            if (it != params.end() && !it->is_null())
                m_scene->SetNavDebugDraw(it->get<bool>());
            resp["enabled"] = m_scene->GetNavDebugDraw();
            resp["note"] = "シーンビューに歩行ポリゴンのワイヤを重ねる"
                           "（明るい線=壁 / 暗い線=ポリゴン同士のポータル）。保存はされない";
        });

    // ════════════════════════════════════════════════════════════
    //  クエリ
    // ════════════════════════════════════════════════════════════
    McpDefine("navmesh_path", "from:vec3,searchHeight:number,searchRadius:number,to:vec3",
              DX12E_MCP_HANDLER
        {
            const nav::NavMesh& nm = m_scene->GetNavMesh();
            if (nm.Empty())
                throw McpError(McpErr::InvalidParam, "navmesh is not built",
                               "先に dx12_navmesh_build で焼くこと");

            const DirectX::XMFLOAT3 a = McpVec3Required(params, "from");
            const DirectX::XMFLOAT3 b = McpVec3Required(params, "to");
            f32 ext[3];
            ReadExtents(params, m_scene->GetNavConfig(), ext);

            const f32 from[3] = { a.x, a.y, a.z };
            const f32 to[3]   = { b.x, b.y, b.z };
            std::vector<f32> path;
            const i32 n = nm.FindPath(from, to, ext, path);

            json pts = json::array();
            f32 len = 0.0f;
            for (size_t i = 0; i + 2 < path.size(); i += 3)
            {
                pts.push_back({ path[i], path[i + 1], path[i + 2] });
                if (i + 5 < path.size())
                {
                    const f32 dx = path[i + 3] - path[i + 0];
                    const f32 dy = path[i + 4] - path[i + 1];
                    const f32 dz = path[i + 5] - path[i + 2];
                    len += std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }

            bool reached = false;
            if (n >= 2)
            {
                const f32 dx = path[path.size() - 3] - to[0];
                const f32 dz = path[path.size() - 1] - to[2];
                reached = std::sqrt(dx * dx + dz * dz) < (std::max)(1.0f, ext[0] * 0.5f);
            }
            resp["pointCount"] = n;
            resp["points"]     = pts;
            resp["length"]     = len;
            resp["reached"]    = reached;
            resp["note"] = reached
                ? "A* + ファネルで求めた通路内の最短折れ線"
                : "目標へ到達できないので「一番近いところまで」を返した（段差/半径で分断されている）";
        });

    McpDefine("navmesh_sample", "point:vec3,searchHeight:number,searchRadius:number", DX12E_MCP_HANDLER
        {
            const nav::NavMesh& nm = m_scene->GetNavMesh();
            if (nm.Empty())
                throw McpError(McpErr::InvalidParam, "navmesh is not built",
                               "先に dx12_navmesh_build で焼くこと");
            const DirectX::XMFLOAT3 p = McpVec3Required(params, "point");
            f32 ext[3];
            ReadExtents(params, m_scene->GetNavConfig(), ext);

            const f32 pos[3] = { p.x, p.y, p.z };
            f32 out[3]{};
            const i32 poly = nm.FindNearestPoly(pos, ext, out);
            resp["onNavMesh"] = poly >= 0;
            if (poly >= 0)
            {
                f32 y = out[1];
                nm.GetHeightAt(out, poly, y);
                resp["poly"]  = poly;
                resp["point"] = { out[0], y, out[2] };
                const f32 dx = out[0] - pos[0], dy = y - pos[1], dz = out[2] - pos[2];
                resp["distance"] = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            resp["note"] = "位置をナビメッシュ上へ落とす（高さは坂道でもボクセル分解能で正確）";
        });

    McpDefine("navmesh_raycast", "from:vec3,searchHeight:number,searchRadius:number,to:vec3",
              DX12E_MCP_HANDLER
        {
            const nav::NavMesh& nm = m_scene->GetNavMesh();
            if (nm.Empty())
                throw McpError(McpErr::InvalidParam, "navmesh is not built",
                               "先に dx12_navmesh_build で焼くこと");
            const DirectX::XMFLOAT3 a = McpVec3Required(params, "from");
            const DirectX::XMFLOAT3 b = McpVec3Required(params, "to");
            f32 ext[3];
            ReadExtents(params, m_scene->GetNavConfig(), ext);

            const f32 from[3] = { a.x, a.y, a.z };
            const f32 to[3]   = { b.x, b.y, b.z };
            f32 start[3]{};
            const i32 poly = nm.FindNearestPoly(from, ext, start);
            if (poly < 0)
                throw McpError(McpErr::InvalidParam, "from is not on the navmesh",
                               "searchRadius / searchHeight を広げるか、from をナビメッシュの上に置くこと");

            f32 t = 1.0f, n[3]{}, hit[3]{};
            const bool blocked = nm.Raycast(start, to, poly, t, n, hit);
            resp["hit"]  = blocked;
            resp["t"]    = t;
            resp["point"]  = { hit[0], hit[1], hit[2] };
            resp["normal"] = { n[0], n[1], n[2] };
            resp["note"] = "ナビメッシュの壁（隣のポリゴンが無い辺）に対する精密な当たり判定。"
                           "AABB ではなく実際の輪郭の辺と交差を取る";
        });
}

} // namespace dx12e
