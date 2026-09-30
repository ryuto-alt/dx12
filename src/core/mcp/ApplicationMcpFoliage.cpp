// ===========================================================================
// MCP: 植生 F1（FoliageLayer + GPU カリング + ExecuteIndirect + 風）
//   foliage_scatter … 地形 / 平面 / メッシュ表面へ「密度・最小間隔（ポアソンディスク）・傾斜・高度・スプラット層」で散布して FoliageLayer を作る
//   foliage_paint   … 円ブラシで追加 / 削除（半径・密度）
//   foliage_clear   … インスタンスを空にする
//   foliage_save    … .dxfoliage を書き出す
//   foliage_stats   … レイヤーごとの個数 / 可視数 / GPU 時間（カリング・描画・影）/ VRAM
//   get_wind / set_wind … シーンの風（SceneWind）
// ---------------------------------------------------------------------------
// ★決定論: seed 指定で毎回同じ結果（OS 非依存の自前 RNG）。既存の dx12_scatter（個別エンティティ・≤200 個・手置き用）は残してある。
// ★undo: FoliageLayer を値で追跡する（_set はコピーオンライトの shared_ptr なので、Undo は古い実体へポインタを戻すだけ）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"
#include "editor/FoliageOps.h"
#include "editor/ScenePick.h"
#include "renderer/foliage/FoliageLayerOps.h"
#include "renderer/foliage/FoliageScatter.h"

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)
using foliage_ops::LayerXform;
using foliage_ops::MakeLayerXform;
using foliage_ops::WorldSurface;

namespace
{
using nlohmann::json;

json Vec3Json(const DirectX::XMFLOAT3& v) { return json::array({v.x, v.y, v.z}); }

foliage::ScatterParams ReadScatterParams(const json& params)
{
    foliage::ScatterParams p;
    p.seed = static_cast<u32>(McpIntParam(params, "seed", 1, 0, 2000000000));
    p.density = McpFloatParam(params, "density", p.density, 0.0f, 1000.0f);
    p.minSpacing = McpFloatParam(params, "minSpacing", p.minSpacing, 0.0f, 1000.0f);
    p.scaleMin = McpFloatParam(params, "scaleMin", p.scaleMin, 0.01f, 100.0f);
    p.scaleMax = McpFloatParam(params, "scaleMax", p.scaleMax, 0.01f, 100.0f);
    if (p.scaleMax < p.scaleMin) std::swap(p.scaleMin, p.scaleMax);
    p.yawRandom = McpFloatParam(params, "yawRandom", p.yawRandom, 0.0f, 1.0f);
    p.tiltAlign = McpFloatParam(params, "tiltAlign", p.tiltAlign, 0.0f, 1.0f);
    p.tiltRandomDeg = McpFloatParam(params, "tiltRandomDeg", p.tiltRandomDeg, 0.0f, 45.0f);
    p.minSlopeDeg = McpFloatParam(params, "minSlopeDeg", p.minSlopeDeg, 0.0f, 90.0f);
    p.maxSlopeDeg = McpFloatParam(params, "maxSlopeDeg", p.maxSlopeDeg, 0.0f, 90.0f);
    p.minHeight = McpFloatParam(params, "minHeight", p.minHeight, -1e6f, 1e6f);
    p.maxHeight = McpFloatParam(params, "maxHeight", p.maxHeight, -1e6f, 1e6f);
    p.splatLayer = McpIntParam(params, "splatLayer", -1, -1, 3);
    p.splatThreshold = McpFloatParam(params, "splatThreshold", p.splatThreshold, 0.0f, 1.0f);
    p.splatPower = McpFloatParam(params, "splatPower", p.splatPower, 0.01f, 16.0f);
    p.colorVariation = McpFloatParam(params, "colorVariation", p.colorVariation, 0.0f, 1.0f);
    p.windVariation = McpFloatParam(params, "windVariation", p.windVariation, 0.0f, 1.0f);
    p.variantCount = static_cast<u32>(McpIntParam(params, "variantCount", 1, 1, 4));
    if (params.contains("variantWeights") && params["variantWeights"].is_array())
    {
        u32 i = 0;
        for (const auto& w : params["variantWeights"])
            if (i < foliage::kMaxVariants && w.is_number()) p.variantWeight[i++] = w.get<f32>();
    }
    p.maxCount = static_cast<u32>(McpIntParam(params, "maxCount", 2000000, 1, 20000000));
    return p;
}
} // namespace

// ハンドラは [this] しか捕まえない DX12E_MCP_HANDLER と同じ形にしつつ、下のローカル補助ラムダ（値でコピーする）も持たせる。
#define FOLIAGE_HANDLER                                                                                           [this, resolveLayer, applyModels, makeSurface, setDirtyAndFlush, layerInfo]                                   ([[maybe_unused]] const nlohmann::json& params, [[maybe_unused]] nlohmann::json& resp,                         [[maybe_unused]] const std::string& method, [[maybe_unused]] McpDeferred& deferred,                           [[maybe_unused]] bool& isDeferred, [[maybe_unused]] bool busyPlaying) -> void

void Application::RegisterMcpFoliageMethods()
{
    // ------------------------------------------------------------------ 共通の部品（ハンドラから使う）
    // 対象のレイヤー（FoliageLayer を持つエンティティ）を引く。無ければ name 指定のときだけ作る（create=true）。
    auto resolveLayer = [this](const json& params, bool create, bool& created) -> entt::entity
    {
        created = false;
        auto& reg = m_scene->GetRegistry();
        const std::string name = params.value("name", std::string());
        entt::entity e = entt::null;
        if (params.contains("entity") && params["entity"].is_number())
        {
            const auto cand = static_cast<entt::entity>(params["entity"].get<u32>());
            if (!reg.valid(cand))
                throw McpError(McpErr::NotFound, "invalid entity id", "dx12_list_entities で取り直すか、name で指定してください");
            e = cand;
        }
        else if (!name.empty())
        {
            const auto ent = m_scene->FindEntity(name);
            if (ent.IsValid()) e = ent.GetHandle();
        }
        else
        {
            // 未指定: FoliageLayer が 1 つだけならそれ
            entt::entity only = entt::null;
            u32 n = 0;
            for (auto [ent, l] : reg.view<FoliageLayer>().each()) { only = ent; ++n; }
            if (n == 1) e = only;
            else throw McpError(McpErr::InvalidParam, n == 0 ? "FoliageLayer がありません" : "FoliageLayer が複数あるので指定が要ります",
                                "name か entity で対象を指定してください（新規作成は name を渡す）");
        }
        if (e == entt::null)
        {
            if (!create)
                throw McpError(McpErr::NotFound, "no entity named '" + name + "'", "dx12_foliage_scatter で先に作ってください");
            // 何も描かない空のエンティティ（NameTag + Transform だけ）。FoliageLayer は下で付ける。
            e = reg.create();
            reg.emplace<NameTag>(e, NameTag{name.empty() ? std::string("Foliage") : name});
            reg.emplace<Transform>(e, Transform{});
            created = true;
        }
        if (!reg.all_of<FoliageLayer>(e))
        {
            if (!create)
                throw McpError(McpErr::InvalidParam, "このエンティティは FoliageLayer を持っていません", "dx12_add_component か dx12_foliage_scatter で付けてください");
            reg.emplace<FoliageLayer>(e);
            created = true;
        }
        return e;
    };

    // モデル指定（variant 文字列）を params から FoliageLayer へ反映する。
    auto applyModels = [](FoliageLayer& l, const json& params)
    {
        // model: 文字列（variant0 の LOD 列）/ models: [「;」区切り文字列, ...] = variant0..3
        if (params.contains("model") && params["model"].is_string()) l.variant0 = params["model"].get<std::string>();
        if (params.contains("models") && params["models"].is_array())
        {
            u32 i = 0;
            for (const auto& m : params["models"])
            {
                if (i >= foliage::kMaxVariants) break;
                if (m.is_string()) foliage::VariantStringMut(l, i) = m.get<std::string>();
                else if (m.is_array())
                {
                    std::vector<std::string> v;
                    for (const auto& s : m) if (s.is_string()) v.push_back(s.get<std::string>());
                    foliage::VariantStringMut(l, i) = foliage::JoinLodList(v);
                }
                ++i;
            }
        }
        auto f = [&](const char* k, f32& v) { if (params.contains(k) && params[k].is_number()) v = params[k].get<f32>(); };
        f("lodDist0", l.lodDist0); f("lodDist1", l.lodDist1); f("lodDist2", l.lodDist2);
        f("cullDistance", l.cullDistance); f("thinStart", l.thinStart); f("lodFade", l.lodFade);
        f("shadowDistance", l.shadowDistance); f("windBend", l.windBend); f("windFlutter", l.windFlutter);
        f("aoStrength", l.aoStrength);
        if (params.contains("castShadow") && params["castShadow"].is_boolean()) l.castShadow = params["castShadow"].get<bool>();
        if (params.contains("shadowMaxLod") && params["shadowMaxLod"].is_number()) l.shadowMaxLod = params["shadowMaxLod"].get<i32>();
        if (params.contains("hzbCulling") && params["hzbCulling"].is_boolean()) l.hzbCulling = params["hzbCulling"].get<bool>();
        if (params.contains("windEnabled") && params["windEnabled"].is_boolean()) l.windEnabled = params["windEnabled"].get<bool>();
    };

    // 表面: terrain / flat / raycast を世界の関数にする（本体は editor/FoliageOps）。
    auto makeSurface = [this](const json& params, entt::entity layerEntity, DirectX::XMFLOAT4& regionWorld, bool& regionGiven,
                              std::string& surfaceDesc) -> WorldSurface
    {
        auto& reg = m_scene->GetRegistry();
        foliage_ops::SurfaceRequest rq;
        rq.mode = params.value("surface", std::string("auto"));
        if (rq.mode != "auto" && rq.mode != "terrain" && rq.mode != "flat" && rq.mode != "raycast")
            throw McpError(McpErr::InvalidParam, "unknown surface: " + rq.mode, "auto / terrain / flat / raycast のどれか", {"auto", "terrain", "flat", "raycast"});
        if (params.contains("terrain") && params["terrain"].is_string())
        {
            const auto ent = m_scene->FindEntity(params["terrain"].get<std::string>());
            if (!ent.IsValid() || !reg.all_of<Terrain>(ent.GetHandle()))
                throw McpError(McpErr::NotFound, "terrain '" + params["terrain"].get<std::string>() + "' が見つかりません", "dx12_list_entities で地形の名前を確かめてください");
            rq.terrain = ent.GetHandle();
        }
        else if (params.contains("terrain") && params["terrain"].is_number())
        {
            const auto cand = static_cast<entt::entity>(params["terrain"].get<u32>());
            if (!reg.valid(cand) || !reg.all_of<Terrain>(cand)) throw McpError(McpErr::NotFound, "terrain の entity id が無効です", "dx12_list_entities で Terrain を持つエンティティの id を確かめてください");
            rq.terrain = cand;
        }
        rq.flatHeight = McpFloatParam(params, "height", 0.0f, -1e6f, 1e6f);
        rq.rayFrom = McpFloatParam(params, "rayFrom", 500.0f, -1e5f, 1e5f);
        if (params.contains("region") && params["region"].is_array() && params["region"].size() == 4)
        {
            rq.hasRegion = true;
            rq.region = {params["region"][0].get<f32>(), params["region"][1].get<f32>(), params["region"][2].get<f32>(), params["region"][3].get<f32>()};
        }
        foliage_ops::BuiltSurface bs;
        std::string err;
        if (!foliage_ops::BuildSurface(reg, &m_drawItems, layerEntity, rq, bs, err))
            throw McpError(McpErr::InvalidParam, err, "surface / terrain / region を確かめてください");
        regionWorld = bs.region;
        regionGiven = bs.regionKnown;
        surfaceDesc = bs.desc;
        return bs.fn;
    };

    auto setDirtyAndFlush = [this](entt::entity e)
    {
        auto& reg = m_scene->GetRegistry();
        auto& l = reg.get<FoliageLayer>(e);
        const NameTag* nt = reg.try_get<NameTag>(e);
        std::string err;
        if (!foliage::FlushSidecar(l, nt ? nt->name : std::string("Foliage"), PathResolver::AssetsDir(), &err, /*force*/ true))
            Logger::Warn("植生: .dxfoliage を書き出せませんでした（{}）", err);
    };

    auto layerInfo = [](entt::registry& reg, entt::entity e) -> json
    {
        json j;
        const auto& l = reg.get<FoliageLayer>(e);
        j["entityId"] = static_cast<u32>(e);
        if (const NameTag* nt = reg.try_get<NameTag>(e)) j["name"] = nt->name;
        j["instances"] = foliage::InstanceCount(l);
        if (l._set)
        {
            j["chunks"] = l._set->chunks.size();
            j["boundsMin"] = json::array({l._set->boundsMin[0], l._set->boundsMin[1], l._set->boundsMin[2]});
            j["boundsMax"] = json::array({l._set->boundsMax[0], l._set->boundsMax[1], l._set->boundsMax[2]});
            u32 byType[foliage::kMaxVariants] = {};
            for (const auto& in : l._set->instances) byType[std::min(foliage::TypeOf(in.seedType), foliage::kMaxVariants - 1)]++;
            j["perVariant"] = json::array({byType[0], byType[1], byType[2], byType[3]});
        }
        j["instancePath"] = l.instancePath;
        j["variant0"] = l.variant0;
        return j;
    };

    // ------------------------------------------------------------------ foliage_scatter
    {
        McpMeta m;
        m.summary  = "植生を散布して FoliageLayer（1 コンポーネント = N 個のインスタンス。エンティティにしない）を作る/更新する。"
                     "地形 / 平面 / メッシュ表面へ、密度・最小間隔（ポアソンディスク）・傾斜・高度・スプラット層・スケール/回転のばらつきで決定論に置く（seed 指定）。"
                     "モデルは model（「;」区切りの LOD 列 例 \"foliage/tree_lod0.glb;foliage/tree_lod1.glb;foliage/tree_lod2.glb\"）か models（種別ごと）";
        m.keywords = "foliage vegetation scatter poisson grass tree 植生 散布 草 木 ポアソン 密度 スプラット 風 instancing 100万 lod";
        m.category = "scene"; m.group = "foliage"; m.target = "scatter";
        m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 120000; m.idempotent = true;
        m.aliases = {"dx12_foliage_scatter"};
        m.params = {
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "レイヤーのエンティティ名（無ければ作る。既定 \"Foliage\"）"),
            P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "既存レイヤーの entity id（name の代わり）"),
            P("model", "string", false, nullptr, nullptr, nullptr, nullptr, "種別 0 の LOD 列（「;」区切り、LOD0 が最詳細。assets 相対）"),
            P("models", "array", false, nullptr, nullptr, nullptr, nullptr, "種別 0..3 ごとの LOD 列（文字列 or 文字列配列）"),
            P("surface", "enum", false, "auto|terrain|flat|raycast", nullptr, nullptr, "\"auto\"", "表面: terrain=地形 / flat=平面（height）/ raycast=シーンのメッシュへ真下レイ / auto=地形があれば地形、無ければ flat"),
            P("terrain", "any", false, nullptr, nullptr, nullptr, nullptr, "地形のエンティティ（名前 or id。省略 = 最初の地形）"),
            P("region", "array", false, nullptr, nullptr, nullptr, nullptr, "[x0,z0,x1,z1]（ワールド XZ。省略 = 地形全面）"),
            P("height", "number", false, nullptr, "-1000000", "1000000", "0", "surface:flat の高さ（ワールド Y）"),
            P("rayFrom", "number", false, nullptr, nullptr, nullptr, "500", "surface:raycast のレイ開始の高さ"),
            P("density", "number", false, nullptr, "0", "1000", "0.5", "平均密度（個/m²）。minSpacing があるときは最大充填のうち何割かに間引く"),
            P("minSpacing", "number", false, nullptr, "0", "1000", "0", "最小間隔 m（ポアソンディスク。0 = 一様ランダム）"),
            P("scaleMin", "number", false, nullptr, "0.01", "100", "0.8", "スケールの下限"),
            P("scaleMax", "number", false, nullptr, "0.01", "100", "1.2", "スケールの上限"),
            P("yawRandom", "number", false, nullptr, "0", "1", "1", "向きのランダム度（1 = 全周）"),
            P("tiltAlign", "number", false, nullptr, "0", "1", "0", "表面の法線へ傾ける割合"),
            P("tiltRandomDeg", "number", false, nullptr, "0", "45", "0", "追加のランダムな傾き（度）"),
            P("minSlopeDeg", "number", false, nullptr, "0", "90", "0", "傾斜の下限（度）"),
            P("maxSlopeDeg", "number", false, nullptr, "0", "90", "90", "傾斜の上限（度）。急斜面に生やさない"),
            P("minHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "高度の下限（ワールド Y）"),
            P("maxHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "高度の上限（ワールド Y）"),
            P("splatLayer", "int", false, nullptr, "-1", "3", "-1", "地形のスプラット層（0..3）の重みで採否を決める（-1 = 使わない）"),
            P("splatThreshold", "number", false, nullptr, "0", "1", "0.3", "この重み未満は置かない"),
            P("colorVariation", "number", false, nullptr, "0", "1", "0.15", "色（明るさ）のばらつき"),
            P("variantCount", "int", false, nullptr, "1", "4", "1", "種別の数（models と揃える）"),
            P("variantWeights", "array", false, nullptr, nullptr, nullptr, nullptr, "種別の出現比"),
            P("seed", "int", false, nullptr, "0", "2000000000", "1", "乱数シード（同じ値 = 同じ結果）"),
            P("maxCount", "int", false, nullptr, "1", "20000000", "2000000", "1 回の散布で作る上限"),
            P("replace", "bool", false, nullptr, nullptr, nullptr, "true", "true = 既存のインスタンスを置き換える / false = 追加（既存との最小間隔も守る）"),
            P("lodDist0", "number", false, nullptr, nullptr, nullptr, nullptr, "レイヤーの LOD0→1 切替距離 m（他の見た目パラメータも同じ名前で受ける）"),
            P("cullDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "描画の最大距離 m"),
        };
        m.next = {{"foliage_stats", "個数・可視数・GPU 時間を読む"}, {"set_wind", "シーンの風を設定する"}, {"foliage_paint", "円ブラシで追加 / 削除"}};
        m.examples = {{"{\"name\":\"Grass\",\"model\":\"foliage/grass_lod0.glb;foliage/grass_lod1.glb\",\"density\":8,\"minSpacing\":0.15,\"seed\":7}", "地形全面に草"},
                      {"{\"name\":\"Trees\",\"model\":\"foliage/tree_lod0.glb;foliage/tree_lod1.glb;foliage/tree_lod2.glb\",\"density\":0.02,\"minSpacing\":4,\"maxSlopeDeg\":25,\"scaleMin\":0.8,\"scaleMax\":1.4}", "木立（緩斜面だけ）"}};
        McpDefine("foliage_scatter", McpMeta(m), FOLIAGE_HANDLER
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は散布できません", "先に dx12_stop で Editor へ戻してください");
                auto& reg = m_scene->GetRegistry();
                bool created = false;
                const entt::entity e = resolveLayer(params, true, created);
                McpUndo().TrackByJsonKey(e, "foliageLayer");
                FoliageLayer& layer = reg.get<FoliageLayer>(e);
                applyModels(layer, params);
                if (layer.variant0.empty())
                    throw McpError(McpErr::InvalidParam, "model が空です", "model に「;」区切りの LOD 列（例 \"foliage/tree_lod0.glb;foliage/tree_lod1.glb\"）を渡してください");

                const foliage::ScatterParams sp = ReadScatterParams(params);
                DirectX::XMFLOAT4 regionW{0, 0, 0, 0};
                bool regionGiven = false;
                std::string surfDesc;
                WorldSurface surf = makeSurface(params, e, regionW, regionGiven, surfDesc);
                if (surfDesc.rfind("terrain", 0) != 0 && !regionGiven)
                    throw McpError(McpErr::InvalidParam, "region が要ります", "[x0,z0,x1,z1]（ワールド XZ）を渡してください");
                const LayerXform lx = MakeLayerXform(reg, e);
                const bool replace = params.value("replace", true);
                const auto t3 = std::chrono::high_resolution_clock::now();
                const foliage_ops::EditResult er = foliage_ops::ScatterLayer(layer, lx, sp, regionW, surf, replace);
                const foliage::ScatterStats& st = er.stats;
                const auto t4 = std::chrono::high_resolution_clock::now();
                setDirtyAndFlush(e);
                const auto t5 = std::chrono::high_resolution_clock::now();
                (void)t3; (void)t4; (void)t5;
                auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
                json r = layerInfo(reg, e);
                r["created"] = created;
                r["surface"] = surfDesc;
                r["scatter"] = {{"candidates", st.candidates}, {"accepted", st.accepted}, {"rejectedSlope", st.rejectedSlope},
                                {"rejectedHeight", st.rejectedHeight}, {"rejectedSplat", st.rejectedSplat},
                                {"rejectedDensity", st.rejectedDensity}, {"rejectedSurface", st.rejectedSurface},
                                {"rejectedSpacing", st.rejectedSpacing}, {"areaM2", st.area}};
                r["timeMs"] = {{"scatter", er.msScatter}, {"chunks", er.msChunks}, {"save", ms(t4, t5)}};
                resp["ok"] = true;
                resp["result"] = r;
            });
    }

    // ------------------------------------------------------------------ foliage_paint
    {
        McpMeta m;
        m.summary  = "円ブラシで植生を追加 / 削除する（半径・密度）。中心はワールド XZ。add は foliage_scatter と同じ規則（表面・傾斜・間隔）で円の中に散布、"
                     "erase は円の中のインスタンスを fraction の割合で消す。points で連続ストローク（最大 512 点）";
        m.keywords = "foliage paint brush erase add 植生 ブラシ 追加 削除 円 半径 密度";
        m.category = "scene"; m.group = "foliage"; m.target = "paint";
        m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 60000;
        m.aliases = {"dx12_foliage_paint"};
        m.params = {
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "レイヤーの名前（省略 = FoliageLayer が 1 つならそれ）"),
            P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "レイヤーの entity id"),
            P("op", "enum", true, "add|erase", nullptr, nullptr, nullptr, "add = 追加 / erase = 削除"),
            P("center", "array", false, nullptr, nullptr, nullptr, nullptr, "[x,z] ワールド XZ"),
            P("points", "array", false, nullptr, nullptr, nullptr, nullptr, "[[x,z],...] 連続ストローク（最大 512 点）"),
            P("radius", "number", false, nullptr, "0.05", "1000", "5", "ブラシ半径 m"),
            P("fraction", "number", false, nullptr, "0", "1", "1", "erase: 消す割合"),
            P("density", "number", false, nullptr, "0", "1000", "0.5", "add: 個/m²"),
            P("minSpacing", "number", false, nullptr, "0", "1000", "0", "add: 最小間隔（既存とも守る）"),
            P("surface", "enum", false, "auto|terrain|flat|raycast", nullptr, nullptr, "\"auto\"", "add: 表面（foliage_scatter と同じ）"),
            P("terrain", "any", false, nullptr, nullptr, nullptr, nullptr, "add: 地形（名前 or id）"),
            P("height", "number", false, nullptr, nullptr, nullptr, "0", "add: surface:flat の高さ"),
            P("seed", "int", false, nullptr, "0", "2000000000", "1", "乱数シード"),
            P("scaleMin", "number", false, nullptr, "0.01", "100", "0.8", "add: スケール下限"), P("scaleMax", "number", false, nullptr, "0.01", "100", "1.2", "add: スケール上限"),
            P("maxSlopeDeg", "number", false, nullptr, "0", "90", "90", "add: 傾斜の上限"), P("splatLayer", "int", false, nullptr, "-1", "3", "-1", "add: スプラット層"),
            P("variantCount", "int", false, nullptr, "1", "4", "1", "add: 種別の数"),
        };
        m.next = {{"foliage_stats", "個数を確かめる"}};
        McpDefine("foliage_paint", McpMeta(m), FOLIAGE_HANDLER
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中はブラシできません", "先に dx12_stop で Editor へ戻してください");
                auto& reg = m_scene->GetRegistry();
                bool created = false;
                const entt::entity e = resolveLayer(params, false, created);
                McpUndo().TrackByJsonKey(e, "foliageLayer");
                FoliageLayer& layer = reg.get<FoliageLayer>(e);
                foliage::EnsureLoaded(layer);
                const std::string op = params.value("op", std::string());
                if (op != "add" && op != "erase") throw McpError(McpErr::InvalidParam, "op は add か erase", "", {"add", "erase"});
                std::vector<std::pair<f32, f32>> centers;
                if (params.contains("points") && params["points"].is_array())
                {
                    if (params["points"].size() > 512) throw McpError(McpErr::InvalidParam, "points は最大 512 点", "ストロークを分けて呼んでください（1 回 512 点まで）");
                    for (const auto& p : params["points"])
                        if (p.is_array() && p.size() >= 2) centers.push_back({p[0].get<f32>(), p[p.size() == 3 ? 2 : 1].get<f32>()});
                }
                else if (params.contains("center") && params["center"].is_array() && params["center"].size() >= 2)
                    centers.push_back({params["center"][0].get<f32>(), params["center"][params["center"].size() == 3 ? 2 : 1].get<f32>()});
                if (centers.empty()) throw McpError(McpErr::InvalidParam, "center か points が要ります", "[x,z]（ワールド XZ）");
                const f32 radius = McpFloatParam(params, "radius", 5.0f, 0.05f, 1000.0f);
                const LayerXform lx = MakeLayerXform(reg, e);
                foliage_ops::EditResult er;
                if (op == "erase")
                {
                    er = foliage_ops::BrushErase(layer, lx, centers, radius, McpFloatParam(params, "fraction", 1.0f, 0.0f, 1.0f),
                                                 static_cast<u32>(McpIntParam(params, "seed", 1, 0, 2000000000)));
                }
                else
                {
                    const foliage::ScatterParams sp = ReadScatterParams(params);
                    DirectX::XMFLOAT4 regionW{0, 0, 0, 0};
                    bool regionGiven = true;
                    std::string surfDesc;
                    json p2 = params;
                    p2["region"] = json::array({centers[0].first - radius, centers[0].second - radius, centers[0].first + radius, centers[0].second + radius});
                    WorldSurface surf = makeSurface(p2, e, regionW, regionGiven, surfDesc);
                    er = foliage_ops::BrushAdd(layer, lx, sp, centers, radius, surf);
                }
                setDirtyAndFlush(e);
                const u32 before = er.before, changed = er.changed;
                json r = layerInfo(reg, e);
                r["op"] = op;
                r["before"] = before;
                r["changed"] = changed;
                resp["ok"] = true;
                resp["result"] = r;
            });
    }

    // ------------------------------------------------------------------ foliage_clear / foliage_save
    {
        McpMeta m;
        m.summary  = "FoliageLayer のインスタンスを全部消す（コンポーネントとモデル指定は残る）。Undo できる";
        m.keywords = "foliage clear 植生 全消去 リセット";
        m.category = "scene"; m.group = "foliage"; m.target = "clear";
        m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 20000; m.idempotent = true;
        m.aliases = {"dx12_foliage_clear"};
        m.params = {P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "レイヤー名"), P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "entity id")};
        McpDefine("foliage_clear", McpMeta(m), FOLIAGE_HANDLER
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は消せません", "先に dx12_stop してください");
                bool created = false;
                const entt::entity e = resolveLayer(params, false, created);
                McpUndo().TrackByJsonKey(e, "foliageLayer");
                auto& layer = m_scene->GetRegistry().get<FoliageLayer>(e);
                foliage::ReplaceSet(layer, std::make_shared<foliage::FoliageInstanceSet>());
                setDirtyAndFlush(e);
                resp["ok"] = true;
                resp["result"] = layerInfo(m_scene->GetRegistry(), e);
            });

        McpMeta s;
        s.summary  = "FoliageLayer の .dxfoliage を書き出す（instancePath が空なら <名前> から決める）。シーン保存でも自動で書かれる";
        s.keywords = "foliage save dxfoliage 保存 植生";
        s.category = "scene"; s.group = "foliage"; s.target = "save";
        s.effect = McpEffect::WriteFile; s.mode = "any"; s.timeoutMs = 20000; s.idempotent = true;
        s.aliases = {"dx12_foliage_save"};
        s.params = {P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "レイヤー名"), P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "entity id")};
        McpDefine("foliage_save", McpMeta(s), FOLIAGE_HANDLER
            {
                bool created = false;
                const entt::entity e = resolveLayer(params, false, created);
                auto& layer = m_scene->GetRegistry().get<FoliageLayer>(e);
                if (!layer._set) throw McpError(McpErr::InvalidParam, "インスタンスがありません", "dx12_foliage_scatter で作ってください");
                setDirtyAndFlush(e);
                resp["ok"] = true;
                resp["result"] = layerInfo(m_scene->GetRegistry(), e);
            });
    }

    // ------------------------------------------------------------------ foliage_stats
    {
        McpMeta m;
        m.summary  = "植生の統計。レイヤーごとの個数・可視数（LOD 別・1〜2 フレーム遅れ）・影の描画数・容量超過・VRAM と、GPU 時間の内訳"
                     "（cull = カリング compute / shadowDraw / depthDraw / mainDraw）。perf_stats の foliage ブロックと同じ";
        m.keywords = "foliage stats perf gpu ms cull 植生 統計 可視 lod 性能 内訳";
        m.category = "render"; m.group = "foliage"; m.target = "stats";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 8000; m.idempotent = true;
        m.aliases = {"dx12_foliage_stats"};
        m.examples = {{"{}", "現在の統計"}};
        McpDefine("foliage_stats", McpMeta(m), DX12E_MCP_HANDLER
            {
                resp["ok"] = true;
                resp["result"] = FoliageStatsJson();
            });
    }

    // ------------------------------------------------------------------ get_wind / set_wind
    {
        McpMeta g;
        g.summary  = "シーンの風（SceneWind）を読む。植生（FoliageLayer）の頂点アニメだけに効く";
        g.keywords = "wind 風 植生 foliage";
        g.category = "render"; g.group = "render_setting"; g.target = "wind";
        g.effect = McpEffect::Read; g.mode = "any"; g.timeoutMs = 5000; g.idempotent = true;
        g.aliases = {"dx12_get_wind"};
        McpDefine("get_wind", McpMeta(g), DX12E_MCP_HANDLER
            {
                const auto& w = m_scene->GetWind();
                resp["ok"] = true;
                resp["result"] = {{"enabled", w.enabled}, {"directionDeg", w.directionDeg}, {"speed", w.speed},
                                  {"gustStrength", w.gustStrength}, {"gustFrequency", w.gustFrequency},
                                  {"turbulence", w.turbulence}, {"phaseOffset", w.phaseOffset}};
            });

        McpMeta s;
        s.summary  = "シーンの風を設定する。directionDeg = 風下の向き（XZ 平面で 0°=+X, 90°=+Z）/ speed = 風速 m/s（0 で無風）/ gustStrength = 突風 0..1 / "
                     "turbulence = 乱れ 0..1 / phaseOffset = 時間オフセット秒（決定論キャプチャで風の位相を選ぶ）。シーン JSON の wind に保存される";
        s.keywords = "wind 風 set 突風 gust 風速 植生 foliage 揺れ";
        s.category = "render"; s.group = "render_setting"; s.target = "wind";
        s.effect = McpEffect::WriteSetting; s.mode = "any"; s.timeoutMs = 5000; s.idempotent = true;
        s.aliases = {"dx12_set_wind"};
        s.params = {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "false で無風"),
            P("directionDeg", "number", false, nullptr, "-100000", "100000", "45", "風が吹いていく向き（度）"),
            P("speed", "number", false, nullptr, "0", "200", "3", "基準の風速 m/s"),
            P("gustStrength", "number", false, nullptr, "0", "1", "0.5", "突風の強さ"),
            P("gustFrequency", "number", false, nullptr, "0", "20", "0.35", "突風が流れる速さ"),
            P("turbulence", "number", false, nullptr, "0", "1", "0.25", "乱れ"),
            P("phaseOffset", "number", false, nullptr, "-1000000", "1000000", "0", "時間のオフセット（秒）"),
        };
        s.next = {{"foliage_stats", "描画への影響を確かめる"}};
        McpDefine("set_wind", McpMeta(s), DX12E_MCP_HANDLER
            {
                auto& w = m_scene->GetWind();
                w.enabled = params.value("enabled", w.enabled);
                w.directionDeg = McpFloatParam(params, "directionDeg", w.directionDeg, -100000.0f, 100000.0f);
                w.speed = McpFloatParam(params, "speed", w.speed, 0.0f, 200.0f);
                w.gustStrength = McpFloatParam(params, "gustStrength", w.gustStrength, 0.0f, 1.0f);
                w.gustFrequency = McpFloatParam(params, "gustFrequency", w.gustFrequency, 0.0f, 20.0f);
                w.turbulence = McpFloatParam(params, "turbulence", w.turbulence, 0.0f, 1.0f);
                w.phaseOffset = McpFloatParam(params, "phaseOffset", w.phaseOffset, -1000000.0f, 1000000.0f);
                resp["ok"] = true;
                resp["result"] = {{"enabled", w.enabled}, {"directionDeg", w.directionDeg}, {"speed", w.speed},
                                  {"gustStrength", w.gustStrength}, {"gustFrequency", w.gustFrequency},
                                  {"turbulence", w.turbulence}, {"phaseOffset", w.phaseOffset}};
            });
    }
}

#undef FOLIAGE_HANDLER

} // namespace dx12e
