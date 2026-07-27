// ===========================================================================
// MCP: ライティング / 診断
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- ライティング / 診断 ----
void Application::RegisterMcpLightingMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    // ════════════════════════════════════════════════════════════
    //  ライティング
    // ════════════════════════════════════════════════════════════
    McpDefine("list_lights", "cursor:int,limit:int", DX12E_MCP_HANDLER
        {
            using namespace DirectX;
            auto& reg = m_scene->GetRegistry();
            const int limit  = McpIntParam(params, "limit", 50, 1, 200);
            const int cursor = McpIntParam(params, "cursor", 0, 0, 100000);

            struct Row { entt::entity e; int kind; int slot; };   // kind 0=dir 1=point 2=spot
            std::vector<Row> rows;
            int dirN = 0, pointN = 0, spotN = 0, shadowSpotN = 0, shadowPointN = 0;
            for (auto [ent, dl, tf] : reg.view<const DirectionalLight, const Transform>().each())
            { (void)dl; (void)tf; rows.push_back({ent, 0, dirN++}); }
            for (auto [ent, pl, tf] : reg.view<const PointLight, const Transform>().each())
            { (void)tf; if (pl.castShadows) ++shadowPointN; rows.push_back({ent, 1, pointN++}); }
            for (auto [ent, sl, tf] : reg.view<const SpotLight, const Transform>().each())
            { (void)tf; if (sl.castShadows) ++shadowSpotN; rows.push_back({ent, 2, spotN++}); }

            json arr = json::array();
            const int total = static_cast<int>(rows.size());
            int i = cursor;
            for (; i < total && static_cast<int>(arr.size()) < limit; ++i)
            {
                const Row& r = rows[static_cast<size_t>(i)];
                XMFLOAT3 wpos{};
                XMStoreFloat3(&wpos, ComputeWorldMatrix(reg, r.e).r[3]);
                json j{
                    {"entityId", static_cast<u32>(r.e)},
                    {"name", reg.all_of<NameTag>(r.e) ? reg.get<NameTag>(r.e).name : std::string()},
                    {"position", {wpos.x, wpos.y, wpos.z}},
                    {"slot", r.slot},
                };
                if (r.kind == 0)
                {
                    const DirectionalLight& dl = reg.get<DirectionalLight>(r.e);
                    const lightmath::SunAngles a = lightmath::DirectionToSunAngles(dl.direction);
                    j["type"]         = "directional";
                    j["color"]        = {dl.color.x, dl.color.y, dl.color.z};
                    j["intensity"]    = dl.intensity;
                    j["ambient"]      = dl.ambient;
                    j["direction"]    = {dl.direction.x, dl.direction.y, dl.direction.z};
                    j["azimuthDeg"]   = a.azimuthDeg;
                    j["elevationDeg"] = a.elevationDeg;
                    j["effective"]    = (r.slot == 0);   // 太陽は先頭 1 灯だけが効く
                    j["overBudget"]   = (r.slot > 0);
                }
                else if (r.kind == 1)
                {
                    const PointLight& pl = reg.get<PointLight>(r.e);
                    j["type"]        = "point";
                    j["color"]       = {pl.color.x, pl.color.y, pl.color.z};
                    j["intensity"]   = pl.intensity;
                    j["range"]       = pl.range;
                    j["castShadows"] = pl.castShadows;
                    // クラスタード化で個別上限は撤廃。overBudget は point+spot 合計の枠で判定する。
                    // Application は「点光源を先、スポットを後」の順で 1 本の配列へ積む。
                    j["overBudget"]  = (r.slot >= kLightBudgetTotal);
                    j["effective"]   = (r.slot < kLightBudgetTotal) && pl.intensity > 0.0f && pl.range > 0.0f;
                }
                else
                {
                    const SpotLight& sl = reg.get<SpotLight>(r.e);
                    j["type"]         = "spot";
                    j["color"]        = {sl.color.x, sl.color.y, sl.color.z};
                    j["intensity"]    = sl.intensity;
                    j["range"]        = sl.range;
                    j["direction"]    = {sl.direction.x, sl.direction.y, sl.direction.z};
                    j["innerConeDeg"] = sl.innerConeDeg;
                    j["outerConeDeg"] = sl.outerConeDeg;
                    j["castShadows"]  = sl.castShadows;
                    // スポットは点光源の後ろに積まれるので通し番号は pointN + slot
                    j["overBudget"]   = (pointN + r.slot >= kLightBudgetTotal);
                    j["effective"]    = (pointN + r.slot < kLightBudgetTotal)
                                      && sl.intensity > 0.0f && sl.range > 0.0f;
                    if (sl.innerConeDeg > sl.outerConeDeg)
                        j["warning"] = "innerConeDeg > outerConeDeg（内外が逆。減衰しない）";
                }
                arr.push_back(std::move(j));
            }

            json warnings = json::array();
            // クラスタードライティング（Forward+）化で「点 8 / スポット 8」の個別上限は撤廃。
            // 今の上限は point + spot の合計 1024 灯。
            if (pointN + spotN > kLightBudgetTotal)
                warnings.push_back("ライトの合計が上限超過 (" + std::to_string(pointN + spotN) + "/"
                    + std::to_string(kLightBudgetTotal) + ")。超えた分は【無言で描画されない】"
                    "（パーティクルの発光ライトも枠を使う）");
            if (pointN + spotN > kLightBudgetPerCluster)
                warnings.push_back("ライトが " + std::to_string(pointN + spotN) + " 灯ある。1 クラスタ("
                    + std::to_string(kLightBudgetPerCluster) + "灯) を超えて重なった所は無言で切り捨てられる。"
                    "密集していないかは【ツール > ライティング > クラスタデバッグ表示】の"
                    "ライト複雑度ヒートマップ(白＝上限に張り付き)で確認すること");
            if (dirN > 1)
                warnings.push_back("平行光が " + std::to_string(dirN) + " 灯ある。太陽として効くのは先頭の 1 灯だけ");
            if (dirN == 0)
                warnings.push_back("平行光(太陽)が無い。dx12_create_entity(type:\"light_directional\") で作れる");
            if (shadowSpotN > kLightBudgetShadowSpot)
                warnings.push_back("影付きスポットが上限超過 (" + std::to_string(shadowSpotN) + "/"
                    + std::to_string(kLightBudgetShadowSpot) + ")。カメラに近い順で選ばれ、残りは影を落とさない");
            if (shadowPointN > kLightBudgetShadowPoint)
                warnings.push_back("影付きポイントが上限超過 (" + std::to_string(shadowPointN) + "/"
                    + std::to_string(kLightBudgetShadowPoint) + ")。カメラに近い順で選ばれ、残りは影を落とさない");

            resp["ok"] = true;
            resp["result"] = {
                {"lights", arr}, {"count", arr.size()}, {"total", total},
                {"cursor", cursor}, {"nextCursor", i}, {"has_more", i < total},
                {"budget", {
                    // クラスタードライティング(Forward+)。point/spot に個別上限は無く、合計 1024 灯。
                    {"total",       {{"used", pointN + spotN}, {"max", kLightBudgetTotal}}},
                    {"perCluster",  {{"max", kLightBudgetPerCluster}}},
                    {"point",       {{"used", pointN},       {"max", kLightBudgetTotal}}},
                    {"spot",        {{"used", spotN},        {"max", kLightBudgetTotal}}},
                    {"directional", {{"used", dirN},         {"max", 1}}},
                    {"shadowSpot",  {{"used", shadowSpotN},  {"max", kLightBudgetShadowSpot}}},
                    {"shadowPoint", {{"used", shadowPointN}, {"max", kLightBudgetShadowPoint}}},
                }},
                {"warnings", warnings},
                {"note", "Transform を持つライトだけを数える(GPU へ送られる条件と同じ)。"
                         "灯数はクラスタードライティングで合計 1024 灯まで(点/スポットの個別上限は無い)。"
                         "ただし影が落ちるのは spot 4 / point 2 のまま。"
                         "太陽の調整は dx12_set_sun、まとめて雰囲気を変えるなら dx12_apply_lighting_preset。"}};
        });

    McpDefine("set_sun", "ambient:number,azimuth:number,color:vec3,elevation:number,intensity:number,"
              "kelvin:number,timeOfDay:number", DX12E_MCP_HANDLER
        {
            auto& reg = m_scene->GetRegistry();
            // view.front() は空なら entt::null。break 付きの for だと MSVC が C4702 を出す。
            const entt::entity sun = reg.view<DirectionalLight>().front();
            if (sun == entt::null)
                throw McpError(McpErr::NotFound, "no DirectionalLight (sun) in this scene",
                    "dx12_create_entity(type:\"light_directional\") で太陽を作ってから呼んでくれ");
            DirectionalLight& dl = reg.get<DirectionalLight>(sun);

            const bool byTime = params.contains("timeOfDay");
            f32 hour = -1.0f;
            if (byTime)
            {
                hour = McpFloatParam(params, "timeOfDay", 12.0f, 0.0f, 24.0f);
                // エディタのスライダ / Lua の Lighting.setTimeOfDay と同じカーブ（LightMath.h）
                const lightmath::TimeOfDaySample s = lightmath::SampleTimeOfDay(hour);
                dl.direction = s.direction;
                dl.color     = s.color;
                dl.intensity = s.intensity;
                dl.ambient   = s.ambient;
            }
            if (params.contains("azimuth") || params.contains("elevation"))
            {
                lightmath::SunAngles a = lightmath::DirectionToSunAngles(dl.direction);
                a.azimuthDeg   = lightmath::WrapDeg180(
                    McpFloatParam(params, "azimuth", a.azimuthDeg, -360.0f, 360.0f));
                a.elevationDeg = McpFloatParam(params, "elevation", a.elevationDeg, -89.0f, 89.0f);
                dl.direction   = lightmath::SunAnglesToDirection(a);
            }
            if (params.contains("kelvin"))
                dl.color = lightmath::KelvinToRGB(McpFloatParam(params, "kelvin", 6500.0f, 1000.0f, 40000.0f));
            DirectX::XMFLOAT3 col{};
            if (McpTryVec3(params, "color", col)) dl.color = col;
            if (params.contains("intensity"))
                dl.intensity = McpFloatParam(params, "intensity", dl.intensity, 0.0f, 100.0f);
            if (params.contains("ambient"))
                dl.ambient = McpFloatParam(params, "ambient", dl.ambient, 0.0f, 5.0f);
            dl._prevRotInit = false;   // Transform 回転の差分追従をリセット

            const lightmath::SunAngles now = lightmath::DirectionToSunAngles(dl.direction);
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(sun)},
                {"name", reg.all_of<NameTag>(sun) ? reg.get<NameTag>(sun).name : std::string()},
                {"direction", {dl.direction.x, dl.direction.y, dl.direction.z}},
                {"azimuthDeg", now.azimuthDeg}, {"elevationDeg", now.elevationDeg},
                {"color", {dl.color.x, dl.color.y, dl.color.z}},
                {"intensity", dl.intensity}, {"ambient", dl.ambient},
                {"timeOfDay", byTime ? json(hour) : json(nullptr)},
                {"note", "絶対指定＝同じ引数の再実行で同じ結果(冪等)。timeOfDay は 0..24 で "
                         "向き/色/強度/環境光を一括で決める(Lua の Lighting.setTimeOfDay と同じカーブ)。"
                         "azimuth/elevation は【太陽が見える方向】(方位 +Z=0°,+X=90° / 高度 0=地平線,90=真上)。"}};
        });

    McpDefine("apply_lighting_preset", "preset:string", DX12E_MCP_HANDLER
        {
            auto& reg = m_scene->GetRegistry();
            std::vector<std::string> presetIds;
            for (int i = 0; i < kLightingPresetCount; ++i) presetIds.push_back(kLightingPresets[i].id);
            const int idx = McpEnumParam(params, "preset", presetIds, -1,
                "エディタの「ライティング」窓のプリセットと同じ実装・同じ値");
            if (idx < 0)
                throw McpError(McpErr::InvalidParam, "missing 'preset'",
                               "どれか 1 つを指定してくれ", presetIds);
            const LightingPreset& p = kLightingPresets[idx];

            // view.front() は空なら entt::null。break 付きの for だと MSVC が C4702 を出す。
            const entt::entity sun = reg.view<DirectionalLight>().front();
            json sunJson = nullptr;
            if (sun != entt::null)
            {
                DirectionalLight& dl = reg.get<DirectionalLight>(sun);
                dl = ApplyLightingPresetToSun(p, dl);
                sunJson = {{"entityId", static_cast<u32>(sun)},
                           {"direction", {dl.direction.x, dl.direction.y, dl.direction.z}},
                           {"color", {dl.color.x, dl.color.y, dl.color.z}},
                           {"intensity", dl.intensity}, {"ambient", dl.ambient}};
            }
            const PostProcessSettings after = ApplyLightingPresetToPost(p, m_scene->GetPostSettings());
            m_scene->GetPostSettings() = after;

            resp["ok"] = true;
            resp["result"] = {
                {"preset", p.id}, {"label", p.label}, {"tip", p.tip},
                {"sun", sunJson},
                {"post", {{"exposureOn", after.exposureOn}, {"exposure", after.exposure},
                          {"bloomOn", after.bloomOn}, {"bloom", after.bloom},
                          {"bloomThreshold", after.bloomThreshold},
                          {"vignetteOn", after.vignetteOn}, {"vignette", after.vignette},
                          {"saturationOn", after.saturationOn}, {"saturation", after.saturation}}},
                {"note", sun == entt::null
                    ? "平行光(太陽)が無いのでポストだけ適用した。dx12_create_entity(type:\"light_directional\") で作れる"
                    : "太陽 + ポストをまとめて適用した(冪等)。細部は dx12_set_sun / dx12_set_post_process で詰める"}};
        });

    // ════════════════════════════════════════════════════════════
    //  エンジン診断（機械可読）
    // ════════════════════════════════════════════════════════════
    McpDefine("diagnose", "only:any", DX12E_MCP_HANDLER
        {
            const std::vector<std::string> allIds = DeepDiag::AllCheckIds();
            std::string only;
            if (params.contains("only") && !params["only"].is_null())
            {
                const auto& o = params["only"];
                std::vector<std::string> want;
                if (o.is_string())
                {
                    std::string s = o.get<std::string>(), cur;
                    std::istringstream ss(s);
                    while (std::getline(ss, cur, ','))
                    {
                        while (!cur.empty() && (cur.front() == ' ')) cur.erase(cur.begin());
                        while (!cur.empty() && (cur.back() == ' '))  cur.pop_back();
                        if (!cur.empty()) want.push_back(cur);
                    }
                }
                else if (o.is_array())
                {
                    for (const auto& v : o)
                        if (v.is_string()) want.push_back(v.get<std::string>());
                }
                else
                {
                    throw McpError(McpErr::InvalidParam, "only must be a string or an array of strings",
                                   "検査 ID をカンマ区切りか配列で渡す", allIds);
                }
                for (const std::string& w : want)
                {
                    if (std::find(allIds.begin(), allIds.end(), w) == allIds.end())
                        throw McpError(McpErr::InvalidParam, "unknown check id: " + w,
                                       "有効な検査 ID のどれかを指定してくれ", allIds);
                    if (!only.empty()) only += ",";
                    only += w;
                }
            }
            json report = DeepDiag::RunAll(*this, only);
            report["checkIds"] = allIds;
            report["note"] = "summary.errors > 0 だけが失敗(注意/情報は失敗ではない)。"
                             "textures/models は assets 全走査で数十秒かかることがあるので、"
                             "速く見たいときは only:\"lighting,terrain,picking,instancing,scripts\"。"
                             "instancing は 1 度も描画していないと測れない(skipped に理由が入る)。";
            resp["ok"] = true;
            resp["result"] = std::move(report);
        });
}



} // namespace dx12e
