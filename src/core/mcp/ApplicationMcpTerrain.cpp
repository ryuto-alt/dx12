// ===========================================================================
// MCP: 地形 / スカルプト
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- 地形 / スカルプト ----
void Application::RegisterMcpTerrainMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    // ════════════════════════════════════════════════════════════
    //  ハイトフィールド地形
    // ════════════════════════════════════════════════════════════
    McpDefine("terrain_create", "color:vec3,maxHeight:number,name:string,position:vec3,resolution:int,uvScale:number,"
              "worldSize:number", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create/modify terrain while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            std::string name = params.value("name", std::string("Terrain"));
            if (name.empty()) name = "Terrain";

            const i32 res  = McpIntParam(params, "resolution", 128, 16, 512);
            const f32 size = McpFloatParam(params, "worldSize", 200.0f, 4.0f, 4000.0f);
            const f32 maxH = McpFloatParam(params, "maxHeight", 200.0f, 1.0f, 10000.0f);

            // 冪等: 同名エンティティが既にあれば「設定の更新」にする（同じ引数の再実行は無変化）。
            entt::entity existing = entt::null;
            for (auto [ent, tag] : reg.view<const NameTag>().each())
                if (tag.name == name) { existing = ent; break; }

            if (existing != entt::null)
            {
                auto* t = reg.try_get<Terrain>(existing);
                if (!t || !t->_hf)
                    throw McpError(McpErr::InvalidParam,
                        "an entity named '" + name + "' already exists and is not a terrain",
                        "別の name を使うか、先に dx12_delete_entity でどけてくれ");

                const u32 newRes = HeightField::NormalizeResolution(static_cast<u32>(res));
                const bool reshape = (newRes != t->_hf->Resolution())
                                  || (std::fabs(t->_hf->WorldSize() - size) > 1e-3f);
                if (reshape) t->_hf->Create(newRes, size, 0.0f);   // 高さ配列は作り直し＝彫った内容は消える

                t->resolution = t->_hf->Resolution();
                t->worldSize  = t->_hf->WorldSize();
                t->maxHeight  = maxH;
                if (params.contains("uvScale"))
                    t->uvScale = McpFloatParam(params, "uvScale", t->uvScale, 0.01f, 1000.0f);
                DirectX::XMFLOAT3 col{};
                if (McpTryVec3(params, "color", col)) t->color = {col.x, col.y, col.z, 1.0f};
                DirectX::XMFLOAT3 pos{};
                if (McpTryVec3(params, "position", pos) && reg.all_of<Transform>(existing))
                    reg.get<Transform>(existing).position = pos;

                if (reshape)
                {
                    const HeightField::Rect full = t->_hf->FullRect();
                    t->MarkDirty(full.x0, full.z0, full.x1, full.z1);
                }
                else
                {
                    t->MarkVisualDirty();
                }
                resp["ok"] = true;
                resp["result"] = {
                    {"entityId", static_cast<u32>(existing)}, {"name", name}, {"created", false},
                    {"resolution", t->resolution}, {"worldSize", t->worldSize},
                    {"maxHeight", t->maxHeight}, {"uvScale", t->uvScale},
                    {"heightsReset", reshape}, {"sceneGeneration", m_sceneGeneration},
                    {"note", reshape ? "解像度/サイズが変わったので高さ配列を作り直した(彫った内容は消えた)"
                                     : "同名の地形があったので設定だけ更新した(高さはそのまま)"}};
            }
            else
            {
                DirectX::XMFLOAT3 pos{0.0f, 0.0f, 0.0f};
                McpTryVec3(params, "position", pos);
                McpPendingProcCreate pending;   // 外側の req(JSON) を隠さないよう別名(C4456)
                pending.kind       = McpPendingProcCreate::Kind::Terrain;
                pending.name       = name;
                pending.position   = pos;
                pending.resolution = static_cast<u32>(res);
                pending.worldSize  = size;
                pending.maxHeight  = maxH;
                pending.mcp        = deferred;
                m_editorCtx->mcpProcCreates.push_back(std::move(pending));
                isDeferred = true;
            }
        });

    McpDefine("terrain_generate", "amplitude:number,baseHeight:number,edgeFalloff:number,entity:int,frequency:number,"
              "name:string,octaves:int,preset:string,ridged:number,seed:int,valleyDepth:number", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot modify terrain while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            Terrain& t = reg.get<Terrain>(e);
            if (!t._hf || !t._hf->IsValid())
                throw McpError(McpErr::Internal, "terrain has no valid height field",
                               "dx12_get_log でロードエラーを確認してくれ");
            HeightField& hf = *t._hf;

            static const std::vector<std::string> kTerrainPresets{"hills", "canyon", "mountains"};
            const int presetIdx = McpEnumParam(params, "preset", kTerrainPresets, 0,
                "hills=なだらかな丘 / canyon=峡谷 / mountains=険しい山脈");
            const u32 seed = static_cast<u32>(McpIntParam(params, "seed", 1337, 0, 2000000000));

            TerrainGenParams gp = MakeTerrainPreset(
                static_cast<TerrainPreset>(presetIdx), hf.WorldSize(), seed);
            gp.frequency   = McpFloatParam(params, "frequency",   gp.frequency,   0.0001f, 1.0f);
            gp.octaves     = McpIntParam  (params, "octaves",     gp.octaves,     1, 8);
            gp.amplitude   = McpFloatParam(params, "amplitude",   gp.amplitude,   0.0f, 10000.0f);
            gp.ridged      = McpFloatParam(params, "ridged",      gp.ridged,      0.0f, 1.0f);
            gp.baseHeight  = McpFloatParam(params, "baseHeight",  gp.baseHeight,  -10000.0f, 10000.0f);
            gp.edgeFalloff = McpFloatParam(params, "edgeFalloff", gp.edgeFalloff, 0.0f, 1.0f);
            gp.valleyDepth = McpFloatParam(params, "valleyDepth", gp.valleyDepth, 0.0f, 10.0f);
            gp.seed        = seed;

            // ★Undo に積みながら書き換える。素で書き換えると Ctrl+Z が
            //   利用者自身の無関係な 1 手を巻き戻す（MarkDirty が .hf の自動保存まで走らせるので
            //   生成そのものは取り消せない）。MarkDirty もこの中で行う。
            TerrainPanel::RunUndoableHeightEdit(reg, *m_editorCtx, e, "Terrain Generate",
                [&](HeightField& h) { GenerateTerrain(h, gp); });

            f32 hMin = 0.0f, hMax = 0.0f;
            hf.MinMaxHeight(hMin, hMax);
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)},
                {"preset", kTerrainPresets[static_cast<size_t>(presetIdx)]},
                {"params", {{"frequency", gp.frequency}, {"octaves", gp.octaves},
                            {"amplitude", gp.amplitude}, {"ridged", gp.ridged},
                            {"baseHeight", gp.baseHeight}, {"edgeFalloff", gp.edgeFalloff},
                            {"valleyDepth", gp.valleyDepth}, {"seed", gp.seed}}},
                {"minHeight", hMin}, {"maxHeight", hMax},
                {"resolution", hf.Resolution()}, {"worldSize", hf.WorldSize()},
                {"note", "高さ配列を丸ごと作り直した(既存の彫りは消える)。同じ seed/params なら毎回同じ地形＝冪等。"
                         "メッシュ・コリジョン・.hf の保存は次のフレームで自動反映される。"}};
        });

    McpDefine("terrain_sculpt", "brush:string,entity:int,falloff:number,flattenHeight:number,mirrorX:bool,mirrorZ:bool,"
              "name:string,noiseFrequency:number,noiseOctaves:int,noiseRidged:number,point:any,"
              "points:any,radius:number,seed:int,strength:number,worldPos:any", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot modify terrain while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            Terrain& t = reg.get<Terrain>(e);
            if (!t._hf || !t._hf->IsValid())
                throw McpError(McpErr::Internal, "terrain has no valid height field");
            HeightField& hf = *t._hf;
            const DirectX::XMFLOAT3 origin = McpTerrainOrigin(reg, e);

            static const std::vector<std::string> kTerrainBrushes{
                "raise", "lower", "smooth", "flatten", "noise"};
            const int bi = McpEnumParam(params, "brush", kTerrainBrushes, 0,
                "raise=盛る / lower=削る / smooth=ならす / flatten=平らに / noise=岩肌。"
                "浸食は dx12_terrain_erode");

            TerrainBrushParams bp;
            bp.type      = static_cast<TerrainBrushType>(bi);
            bp.radius    = McpFloatParam(params, "radius",   12.0f, 0.01f, hf.WorldSize());
            bp.strength  = McpFloatParam(params, "strength",  5.0f, 0.0f, 10000.0f);
            bp.falloff   = McpFloatParam(params, "falloff",   0.5f, 0.0f, 1.0f);
            bp.minHeight = -t.maxHeight;
            bp.maxHeight =  t.maxHeight;
            bp.mirrorX   = params.value("mirrorX", false);
            bp.mirrorZ   = params.value("mirrorZ", false);
            bp.noiseFrequency = McpFloatParam(params, "noiseFrequency", bp.noiseFrequency, 0.0001f, 10.0f);
            bp.noiseOctaves   = McpIntParam  (params, "noiseOctaves",   bp.noiseOctaves,   1, 8);
            bp.noiseRidged    = McpFloatParam(params, "noiseRidged",    bp.noiseRidged,    0.0f, 1.0f);
            bp.noiseSeed      = static_cast<u32>(McpIntParam(params, "seed", 1337, 0, 2000000000));

            // 筆を置く点（ワールド XZ）。1 点は point/worldPos、稜線や道を引くなら points。
            std::vector<DirectX::XMFLOAT2> pts;
            auto pushPoint = [&](const json& a) {
                if (!a.is_array() || (a.size() != 2 && a.size() != 3))
                    throw McpError(McpErr::InvalidParam, "point must be [x,z] or [x,y,z]",
                                   "ワールド座標。y は使わない(地形は XZ グリッドなので)");
                const f32 wx = a[0].get<f32>();
                const f32 wz = (a.size() == 2) ? a[1].get<f32>() : a[2].get<f32>();
                pts.push_back({wx - origin.x, wz - origin.z});   // → 地形ローカル
            };
            if (params.contains("points"))
            {
                if (!params["points"].is_array())
                    throw McpError(McpErr::InvalidParam, "points must be an array of [x,z]",
                                   "1 点だけなら point:[x,z] を使う");
                if (params["points"].size() > 512)
                    throw McpError(McpErr::InvalidParam, "too many points (max 512)",
                                   "分割して複数回呼んでくれ");
                for (const auto& a : params["points"]) pushPoint(a);
            }
            if (params.contains("point"))    pushPoint(params["point"]);
            if (params.contains("worldPos")) pushPoint(params["worldPos"]);
            if (pts.empty())
                throw McpError(McpErr::InvalidParam, "missing 'point' / 'points'",
                    "point:[x,z] か points:[[x,z],...] をワールド座標で渡す。"
                    "置きたい場所が分からんときは dx12_pick か dx12_terrain_sample で調べる");

            // Flatten の目標高さ: 明示が無ければ最初の点の高さ（エディタのストローク開始と同じ規約）
            bp.flattenTarget = McpFloatParam(params, "flattenHeight",
                                             hf.SampleHeight(pts[0].x, pts[0].y) + 0.0f,
                                             -10000.0f, 10000.0f);
            if (params.contains("flattenHeight"))
                bp.flattenTarget -= origin.y;   // 指定はワールド Y。地形ローカルの高さへ直す

            HeightField::Rect changed{};
            TerrainPanel::RunUndoableHeightEdit(reg, *m_editorCtx, e, "Terrain Sculpt",
                [&](HeightField& h)
                {
                    for (const DirectX::XMFLOAT2& p : pts)
                        changed = HeightField::UnionRect(
                            changed, ApplyTerrainBrush(h, bp, p.x, p.y, 1.0f, false));
                });

            f32 hMin = 0.0f, hMax = 0.0f;
            hf.MinMaxHeight(hMin, hMax);
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)},
                {"brush", kTerrainBrushes[static_cast<size_t>(bi)]},
                {"points", pts.size()}, {"radius", bp.radius}, {"strength", bp.strength},
                {"changed", changed.Valid()},
                {"minHeight", hMin}, {"maxHeight", hMax},
                {"note", "★相対操作（同じ呼び出しを2回撃つと2回ぶん盛れる）。絶対値で整地したいときは "
                         "brush:\"flatten\" + flattenHeight を使うと冪等になる。"
                         "strength は raise/lower/noise = メートル、smooth/flatten = 寄せ具合(2 でほぼ完全)。"}};
        });

    McpDefine("terrain_paint|terrain_autopaint", "dirtSlopeEnd:number,dirtSlopeStart:number,entity:int,falloff:number,layer:int,"
              "name:string,noiseStrength:number,point:any,points:any,radius:number,"
              "rockSlopeEnd:number,rockSlopeStart:number,snowHeightEnd:number,snowHeightStart:number,"
              "strength:number,worldPos:any", DX12E_MCP_HANDLER
        {
            // 地形のテクスチャレイヤーを塗る（terrain_sculpt の写経）。
            //   terrain_paint     … 円ブラシで 1 レイヤーの重みを上げる（相対操作）
            //   terrain_autopaint … 傾斜と標高から 4 層を焼き直す（冪等）
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot modify terrain while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            Terrain& t = reg.get<Terrain>(e);
            if (!t._hf || !t._hf->IsValid())
                throw McpError(McpErr::Internal, "terrain has no valid height field");
            if (t.layerSetPath.empty())
                throw McpError(McpErr::InvalidParam, "terrain has no layer set",
                    "先に地形ツール窓（またはシーン JSON）で terrain.layerSetPath へ "
                    ".terrainlayers を割り当ててくれ。空のままだと従来の頂点色描画のまま");

            // スプラットが無ければここで作る（シーンを開き直さずに使えるようにする）
            if (!t._splat)
            {
                t._splat = std::make_shared<TerrainSplatMap>();
                t._splat->Create(t.splatResolution);
                t._splat->AutoPaintFromHeightField(*t._hf, TerrainAutoPaintParams{});
                t.splatResolution = t._splat->Size();
            }
            TerrainSplatMap& sp = *t._splat;
            const DirectX::XMFLOAT3 origin = McpTerrainOrigin(reg, e);

            if (method == "terrain_autopaint")
            {
                TerrainAutoPaintParams ap;
                ap.rockSlopeStart  = McpFloatParam(params, "rockSlopeStart",  ap.rockSlopeStart,  0.0f, 1.0f);
                ap.rockSlopeEnd    = McpFloatParam(params, "rockSlopeEnd",    ap.rockSlopeEnd,    0.0f, 1.0f);
                ap.dirtSlopeStart  = McpFloatParam(params, "dirtSlopeStart",  ap.dirtSlopeStart,  0.0f, 1.0f);
                ap.dirtSlopeEnd    = McpFloatParam(params, "dirtSlopeEnd",    ap.dirtSlopeEnd,    0.0f, 1.0f);
                ap.noiseStrength   = McpFloatParam(params, "noiseStrength",   ap.noiseStrength,   0.0f, 1.0f);
                if (params.contains("snowHeightStart") || params.contains("snowHeightEnd"))
                {
                    ap.autoSnowHeight   = false;
                    ap.snowHeightStart  = McpFloatParam(params, "snowHeightStart", 0.0f, -10000.0f, 10000.0f) - origin.y;
                    ap.snowHeightEnd    = McpFloatParam(params, "snowHeightEnd",  50.0f, -10000.0f, 10000.0f) - origin.y;
                }
                // ★Undo に積みながら塗る（高さ側と同じ理由）。_splatNeedsSave もこの中で立つ。
                TerrainPanel::RunUndoableSplatEdit(reg, *m_editorCtx, e,
                    [&](TerrainSplatMap& m) { m.AutoPaintFromHeightField(*t._hf, ap); });

                resp["ok"] = true;
                resp["result"] = {
                    {"entityId", static_cast<u32>(e)}, {"splatSize", sp.Size()},
                    {"note", "★冪等（何度呼んでも同じ結果）。手で塗った内容は上書きされる。"
                             "しきい値は傾斜 0=平ら〜1=垂直、標高はワールド Y(m)。"}};
            }
            else
            {
                const int layer = McpIntParam(params, "layer", 0, 0, 3);
                const f32 radius   = McpFloatParam(params, "radius",   12.0f, 0.01f, t._hf->WorldSize());
                const f32 strength = McpFloatParam(params, "strength", 0.7f,  0.0f, 1.0f);
                const f32 falloff  = McpFloatParam(params, "falloff",  0.5f,  0.0f, 1.0f);

                std::vector<DirectX::XMFLOAT2> pts;
                auto pushPoint = [&](const nlohmann::json& a) {
                    if (!a.is_array() || (a.size() != 2 && a.size() != 3))
                        throw McpError(McpErr::InvalidParam, "point must be [x,z] or [x,y,z]",
                                       "ワールド座標。y は使わない(地形は XZ グリッドなので)");
                    const f32 wx = a[0].get<f32>();
                    const f32 wz = (a.size() == 2) ? a[1].get<f32>() : a[2].get<f32>();
                    pts.push_back({wx - origin.x, wz - origin.z});
                };
                if (params.contains("points"))
                {
                    if (!params["points"].is_array())
                        throw McpError(McpErr::InvalidParam, "points must be an array of [x,z]");
                    if (params["points"].size() > 512)
                        throw McpError(McpErr::InvalidParam, "too many points (max 512)");
                    for (const auto& a : params["points"]) pushPoint(a);
                }
                if (params.contains("point"))    pushPoint(params["point"]);
                if (params.contains("worldPos")) pushPoint(params["worldPos"]);
                if (pts.empty())
                    throw McpError(McpErr::InvalidParam, "missing 'point' / 'points'",
                        "point:[x,z] か points:[[x,z],...] をワールド座標で渡す");

                bool changed = false;
                TerrainPanel::RunUndoableSplatEdit(reg, *m_editorCtx, e,
                    [&](TerrainSplatMap& m)
                    {
                        for (const DirectX::XMFLOAT2& p : pts)
                            changed |= m.PaintCircle(t._hf->WorldSize(), p.x, p.y,
                                                     radius, static_cast<u32>(layer),
                                                     strength, falloff).Valid();
                    });

                resp["ok"] = true;
                resp["result"] = {
                    {"entityId", static_cast<u32>(e)}, {"layer", layer},
                    {"points", pts.size()}, {"radius", radius}, {"strength", strength},
                    {"changed", changed}, {"splatSize", sp.Size()},
                    {"note", "★相対操作（同じ呼び出しを2回撃つと2回ぶん塗れる）。strength=1 で 1 回塗れば"
                             "そのレイヤー 100%。他レイヤーは合計 1 を保つよう比例縮小される。"
                             "全面を傾斜/標高から焼き直すなら dx12_terrain_autopaint。"}};
            }
        });

    McpDefine("terrain_set_layers", "autopaint:bool,distTiling:any,distTilingFarScale:number,distTilingStart:number,"
              "entity:int,heightBlendDepth:number,layerSetPath:any,macro:any,macroScale:number,"
              "macroStrength:number,name:string,normalStrength:number,pom:any,pomFadeEnd:number,"
              "pomFadeStart:number,pomHeightScale:number,splatResolution:int,triplanar:any,"
              "triplanarSharpness:number,uvScale:number", DX12E_MCP_HANDLER
        {
            // 地形へ .terrainlayers（4 層の PBR 素材）を割り当てる / 外す。
            // ★これが無かったので terrain_paint / autopaint は「シーン JSON を手書きして
            //   開き直した地形」にしか使えなかった（#27）。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot modify terrain while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            Terrain& t = reg.get<Terrain>(e);
            if (!t._hf || !t._hf->IsValid())
                throw McpError(McpErr::Internal, "terrain has no valid height field");

            if (!params.contains("layerSetPath"))
                throw McpError(McpErr::InvalidParam, "missing 'layerSetPath'",
                    "assets 相対の .terrainlayers。空文字 \"\" を渡すと割当を外して従来の見た目へ戻す");
            const std::string prev = t.layerSetPath;
            std::string relPath = params["layerSetPath"].get<std::string>();

            u32  layerCount = 0;
            json layerNames = json::array();
            if (!relPath.empty())
            {
                relPath = ValidateMcpAssetRelPath(relPath, "layerSetPath");
                if (!vfs::Exists(relPath))
                    throw McpError(McpErr::NotFound, "layer set not found: " + relPath,
                        "assets 相対で .terrainlayers を指す（例 terrain/alpine.terrainlayers）");
                TerrainLayerSetData lsData;
                if (!ParseTerrainLayerSet(vfs::ReadAsset(relPath), lsData))
                    throw McpError(McpErr::InvalidParam, "failed to parse " + relPath,
                        "JSON の形式が違う。既存の .terrainlayers を雛形にしてくれ");
                layerCount = static_cast<u32>((std::min)(lsData.layers.size(), size_t{4}));
                for (u32 i = 0; i < layerCount; ++i) layerNames.push_back(lsData.layers[i].name);
            }
            t.layerSetPath = relPath;

            // 各種パラメータ（省略したものは触らない）
            if (params.contains("splatResolution"))
                t.splatResolution = TerrainSplatMap::NormalizeSize(
                    static_cast<u32>(McpIntParam(params, "splatResolution", 512, 32, 2048)));
            if (params.contains("uvScale"))
                t.uvScale = McpFloatParam(params, "uvScale", t.uvScale, 0.01f, 1000.0f);
            if (params.contains("heightBlendDepth"))
                t.heightBlendDepth = McpFloatParam(params, "heightBlendDepth", t.heightBlendDepth, 0.01f, 1.0f);
            if (params.contains("triplanarSharpness"))
                t.triplanarSharpness = McpFloatParam(params, "triplanarSharpness", t.triplanarSharpness, 1.0f, 16.0f);
            if (params.contains("normalStrength"))
                t.normalStrength = McpFloatParam(params, "normalStrength", t.normalStrength, 0.0f, 2.0f);
            if (params.contains("macroScale"))
                t.macroScale = McpFloatParam(params, "macroScale", t.macroScale, 10.0f, 400.0f);
            if (params.contains("macroStrength"))
                t.macroStrength = McpFloatParam(params, "macroStrength", t.macroStrength, 0.0f, 1.0f);
            if (params.contains("distTilingStart"))
                t.distTilingStart = McpFloatParam(params, "distTilingStart", t.distTilingStart, 5.0f, 200.0f);
            if (params.contains("distTilingFarScale"))
                t.distTilingFarScale = McpFloatParam(params, "distTilingFarScale", t.distTilingFarScale, 2.0f, 16.0f);
            if (params.contains("pomHeightScale"))
                t.pomHeightScale = McpFloatParam(params, "pomHeightScale", t.pomHeightScale, 0.0f, 0.3f);
            if (params.contains("pomFadeStart"))
                t.pomFadeStart = McpFloatParam(params, "pomFadeStart", t.pomFadeStart, 0.0f, 40.0f);
            if (params.contains("pomFadeEnd"))
                t.pomFadeEnd = McpFloatParam(params, "pomFadeEnd", t.pomFadeEnd, 1.0f, 120.0f);
            {
                // bit0=triplanar bit1=pom bit2=macro bit3=distTiling
                u32 flags = t.terrainMatFlags;
                auto setBit = [&](const char* key, u32 bit) {
                    if (params.contains(key) && params[key].is_boolean())
                        flags = params[key].get<bool>() ? (flags | bit) : (flags & ~bit);
                };
                setBit("triplanar", 0x1u); setBit("pom", 0x2u);
                setBit("macro", 0x4u);     setBit("distTiling", 0x8u);
                t.terrainMatFlags = flags;
            }

            // 割当の付け外しでスプラットを用意/破棄する（TerrainPanel と同じ規則）。
            bool splatCreated = false;
            if (t.layerSetPath.empty())
            {
                t._splat.reset();
            }
            else if (!t._splat)
            {
                t._splat = std::make_shared<TerrainSplatMap>();
                if (t.splatPath.empty() || !terrain::LoadSplatMapAsset(t.splatPath, *t._splat))
                {
                    t._splat->Create(t.splatResolution);
                    if (params.value("autopaint", true))
                        t._splat->AutoPaintFromHeightField(*t._hf, TerrainAutoPaintParams{});
                    t._splatNeedsSave = true;
                    splatCreated = true;
                }
                t.splatResolution = t._splat->Size();
            }
            t.MarkVisualDirty();

            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)},
                {"layerSetPath", t.layerSetPath},
                {"previousLayerSetPath", prev},
                {"layerCount", layerCount},
                {"layerNames", layerNames},
                {"splatPath", t.splatPath},
                {"splatSize", t._splat ? t._splat->Size() : 0u},
                {"splatCreated", splatCreated},
                {"uvScale", t.uvScale},
                {"terrainMatFlags", t.terrainMatFlags},
                {"sceneGeneration", m_sceneGeneration},
                {"note", t.layerSetPath.empty()
                    ? "レイヤーセットを外した（従来の頂点色 / .dxmat 経路へ戻る）"
                    : "割当てた。スプラットは .splat へ自動保存される。"
                      "塗るのは dx12_terrain_paint / dx12_terrain_autopaint、"
                      "結果の確認は dx12_terrain_splat_info。"}};
        });

    McpDefine("terrain_splat_info", "entity:int,gridSize:int,name:string,point:any,points:any", DX12E_MCP_HANDLER
        {
            // スプラット（レイヤー重み）の要約を返す読み取り専用メソッド。
            // terrain_paint / autopaint の結果を「絵を見ずに」検証するために使う。
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            Terrain& t = reg.get<Terrain>(e);

            json out;
            out["entityId"]     = static_cast<u32>(e);
            out["layerSetPath"] = t.layerSetPath;
            out["splatPath"]    = t.splatPath;
            out["hasSplat"]     = static_cast<bool>(t._splat && t._splat->IsValid());
            out["unsavedSplat"] = t._splatNeedsSave;
            if (!t._splat || !t._splat->IsValid())
            {
                out["splatSize"] = 0;
                out["note"] = "この地形にはまだスプラットが無い。dx12_terrain_set_layers で "
                              ".terrainlayers を割り当てると作られる";
                resp["ok"] = true;
                resp["result"] = std::move(out);
            }
            else
            {
                const TerrainSplatMap& sp = *t._splat;
                const u32 size = sp.Size();
                const std::vector<u8>& px = sp.Pixels();
                out["splatSize"] = size;

                // 全面の平均重み(0..1) と「最大レイヤー」のテクセル数
                double sum[4] = {0, 0, 0, 0};
                u64    domCount[4] = {0, 0, 0, 0};
                const u64 total = static_cast<u64>(size) * size;
                for (u64 i = 0; i < total; ++i)
                {
                    const u8* p = &px[static_cast<size_t>(i) * 4];
                    u32 best = 0;
                    for (u32 c = 0; c < 4; ++c)
                    {
                        sum[c] += p[c];
                        if (p[c] > p[best]) best = c;
                    }
                    ++domCount[best];
                }
                json coverage = json::array(), dominant = json::array();
                for (u32 c = 0; c < 4; ++c)
                {
                    coverage.push_back(sum[c] / (255.0 * static_cast<double>(total)));
                    dominant.push_back(static_cast<double>(domCount[c]) / static_cast<double>(total));
                }
                out["coverage"]      = coverage;   // 平均重み（4 層の合計はほぼ 1）
                out["dominantRatio"] = dominant;   // そのレイヤーが最大だったテクセルの割合

                // 粗いグリッド（各セルの支配レイヤー番号）。paint がどこに乗ったかを目で追える。
                const i32 g = McpIntParam(params, "gridSize", 8, 0, 32);
                if (g > 0)
                {
                    json grid = json::array();
                    for (i32 gz = 0; gz < g; ++gz)
                    {
                        std::string row;
                        for (i32 gx = 0; gx < g; ++gx)
                        {
                            const u32 x0 = static_cast<u32>(static_cast<u64>(gx)     * size / static_cast<u32>(g));
                            const u32 x1 = static_cast<u32>(static_cast<u64>(gx + 1) * size / static_cast<u32>(g));
                            const u32 z0 = static_cast<u32>(static_cast<u64>(gz)     * size / static_cast<u32>(g));
                            const u32 z1 = static_cast<u32>(static_cast<u64>(gz + 1) * size / static_cast<u32>(g));
                            u64 acc[4] = {0, 0, 0, 0};
                            for (u32 z = z0; z < z1; ++z)
                                for (u32 x = x0; x < x1; ++x)
                                {
                                    const u8* p = &px[(static_cast<size_t>(z) * size + x) * 4];
                                    for (u32 c = 0; c < 4; ++c) acc[c] += p[c];
                                }
                            u32 best = 0;
                            for (u32 c = 1; c < 4; ++c) if (acc[c] > acc[best]) best = c;
                            row.push_back(static_cast<char>('0' + best));
                        }
                        grid.push_back(row);
                    }
                    out["gridSize"] = g;
                    out["grid"]     = grid;   // grid[z][x] = 支配レイヤー番号。z が増えると +Z
                }

                // 指定ワールド座標の正確な重み
                if (params.contains("point") || params.contains("points"))
                {
                    const DirectX::XMFLOAT3 origin = McpTerrainOrigin(reg, e);
                    const f32 worldSize = t._hf ? t._hf->WorldSize() : t.worldSize;
                    json samples = json::array();
                    auto sample = [&](const nlohmann::json& a) {
                        if (!a.is_array() || (a.size() != 2 && a.size() != 3))
                            throw McpError(McpErr::InvalidParam, "point must be [x,z] or [x,y,z]");
                        const f32 wx = a[0].get<f32>();
                        const f32 wz = (a.size() == 2) ? a[1].get<f32>() : a[2].get<f32>();
                        const i32 tx = sp.LocalToTexelX(wx - origin.x, worldSize);
                        const i32 tz = sp.LocalToTexelZ(wz - origin.z, worldSize);
                        const u8* p = &px[(static_cast<size_t>(tz) * size + static_cast<size_t>(tx)) * 4];
                        u32 best = 0;
                        for (u32 c = 1; c < 4; ++c) if (p[c] > p[best]) best = c;
                        samples.push_back({{"world", {wx, wz}}, {"texel", {tx, tz}},
                                           {"weights", {p[0] / 255.0, p[1] / 255.0,
                                                        p[2] / 255.0, p[3] / 255.0}},
                                           {"dominant", best}});
                    };
                    if (params.contains("points"))
                    {
                        if (!params["points"].is_array() || params["points"].size() > 256)
                            throw McpError(McpErr::InvalidParam, "points must be an array (max 256)");
                        for (const auto& a : params["points"]) sample(a);
                    }
                    if (params.contains("point")) sample(params["point"]);
                    out["samples"] = samples;
                }
                out["note"] = "coverage/dominantRatio は 0..1。grid[z][x] は '0'..'3' の文字で"
                              "そのセルの支配レイヤー（z が増えると +Z、x が増えると +X）。";
                resp["ok"] = true;
                resp["result"] = std::move(out);
            }
        });

    McpDefine("terrain_erode", "entity:int,iterations:int,name:string,region:any,talusDeg:number", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot modify terrain while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            Terrain& t = reg.get<Terrain>(e);
            if (!t._hf || !t._hf->IsValid())
                throw McpError(McpErr::Internal, "terrain has no valid height field");
            HeightField& hf = *t._hf;
            const DirectX::XMFLOAT3 origin = McpTerrainOrigin(reg, e);

            const i32 iterations = McpIntParam(params, "iterations", 16, 1, 200);
            const f32 talusDeg   = McpFloatParam(params, "talusDeg", 34.0f, 1.0f, 89.0f);

            HeightField::Rect rect = hf.FullRect();
            if (params.contains("region"))
            {
                const auto& r = params["region"];
                if (!r.is_array() || r.size() != 4)
                    throw McpError(McpErr::InvalidParam, "region must be [minX,minZ,maxX,maxZ]",
                                   "ワールド座標の XZ 矩形。省略すると地形全面");
                const f32 cell = hf.CellSize();
                const f32 half = hf.HalfSize();
                auto toGrid = [&](f32 world, f32 originAxis) {
                    return (world - originAxis + half) / ((cell > 1e-6f) ? cell : 1.0f);
                };
                HeightField::Rect want;
                want.x0 = static_cast<i32>(std::floor(toGrid(r[0].get<f32>(), origin.x)));
                want.z0 = static_cast<i32>(std::floor(toGrid(r[1].get<f32>(), origin.z)));
                want.x1 = static_cast<i32>(std::ceil (toGrid(r[2].get<f32>(), origin.x)));
                want.z1 = static_cast<i32>(std::ceil (toGrid(r[3].get<f32>(), origin.z)));
                rect = hf.ClampRect(want);
                if (!rect.Valid())
                    throw McpError(McpErr::InvalidParam, "region is outside the terrain",
                                   "地形の XZ 範囲は dx12_terrain_sample で確認できる");
            }

            HeightField::Rect changed{};
            TerrainPanel::RunUndoableHeightEdit(reg, *m_editorCtx, e, "Terrain Erode",
                [&](HeightField& h) { changed = ApplyThermalErosion(h, talusDeg, iterations, rect); });

            f32 hMin = 0.0f, hMax = 0.0f;
            hf.MinMaxHeight(hMin, hMax);
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)}, {"iterations", iterations},
                {"talusDeg", talusDeg}, {"changed", changed.Valid()},
                {"minHeight", hMin}, {"maxHeight", hMax},
                {"note", "熱浸食(安息角を超えた斜面の土砂を隣へ落とす)。相対操作なので繰り返すほど崩れる。"
                         "重いので iterations は 16〜40 くらいから試すとええ。"}};
        });

    McpDefine("terrain_sample", "entity:int,name:string,points:any", DX12E_MCP_HANDLER
        {
            // 読み取り専用。木や建物を「地形の上」に置くための高さ/法線問い合わせ。
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<Terrain>(
                *m_scene, params, "terrain", "先に dx12_terrain_create で地形を作ってくれ");
            const Terrain& t = reg.get<Terrain>(e);
            if (!t._hf || !t._hf->IsValid())
                throw McpError(McpErr::Internal, "terrain has no valid height field");
            const HeightField& hf = *t._hf;
            const DirectX::XMFLOAT3 origin = McpTerrainOrigin(reg, e);
            const f32 half = hf.HalfSize();

            json samples = json::array();
            if (params.contains("points"))
            {
                const auto& arr = params["points"];
                if (!arr.is_array())
                    throw McpError(McpErr::InvalidParam, "points must be an array of [x,z]");
                if (arr.size() > 512)
                    throw McpError(McpErr::InvalidParam, "too many points (max 512)",
                                   "分割して複数回呼んでくれ");
                for (const auto& a : arr)
                {
                    if (!a.is_array() || (a.size() != 2 && a.size() != 3))
                        throw McpError(McpErr::InvalidParam, "each point must be [x,z] or [x,y,z]");
                    const f32 wx = a[0].get<f32>();
                    const f32 wz = (a.size() == 2) ? a[1].get<f32>() : a[2].get<f32>();
                    const f32 lx = wx - origin.x, lz = wz - origin.z;
                    const f32 h  = hf.SampleHeight(lx, lz);
                    const DirectX::XMFLOAT3 n = hf.SampleNormal(lx, lz);
                    samples.push_back({
                        {"x", wx}, {"z", wz},
                        {"height", h}, {"worldY", origin.y + h},
                        {"normal", {n.x, n.y, n.z}},
                        {"slopeDeg", DirectX::XMConvertToDegrees(
                            std::acos(std::clamp(n.y, -1.0f, 1.0f)))},
                        {"inside", (std::fabs(lx) <= half && std::fabs(lz) <= half)},
                    });
                }
            }
            f32 hMin = 0.0f, hMax = 0.0f;
            hf.MinMaxHeight(hMin, hMax);
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)},
                {"name", reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string()},
                {"origin", {origin.x, origin.y, origin.z}},
                {"resolution", hf.Resolution()}, {"worldSize", hf.WorldSize()},
                {"cellSize", hf.CellSize()},
                {"boundsXZ", {origin.x - half, origin.z - half, origin.x + half, origin.z + half}},
                {"minHeight", hMin}, {"maxHeight", hMax},
                {"samples", samples}, {"count", samples.size()},
                {"note", "worldY をそのまま dx12_set_transform の y に使えば地形に接地する"
                         "(範囲外は端の値。inside=false で判別)。"}};
        });

    // ════════════════════════════════════════════════════════════
    //  頂点スカルプト（洞窟・アーチ・岩みたいな異形）
    // ════════════════════════════════════════════════════════════
    McpDefine("sculpt_create", "collision:any,color:vec3,name:string,position:vec3,primitive:string,size:number,"
              "subdivisions:int,uvScale:number", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create a sculpt mesh while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            std::string name = params.value("name", std::string("Sculpt"));
            if (name.empty()) name = "Sculpt";

            static const std::vector<std::string> kSculptPrims{"box", "sphere", "plane", "cylinder"};
            const int prim   = McpEnumParam(params, "primitive", kSculptPrims, 1,
                "岩は sphere、アーチ/柱は cylinder、崖は box、地面の一部は plane から彫るのが早い");
            const i32 subdiv = McpIntParam(params, "subdivisions", 16, 1, 64);
            const f32 size   = McpFloatParam(params, "size", 2.0f, 0.05f, 500.0f);

            entt::entity existing = entt::null;
            for (auto [ent, tag] : reg.view<const NameTag>().each())
                if (tag.name == name) { existing = ent; break; }

            if (existing != entt::null)
            {
                auto* sc = reg.try_get<SculptMesh>(existing);
                if (!sc)
                    throw McpError(McpErr::InvalidParam,
                        "an entity named '" + name + "' already exists and is not a sculpt mesh",
                        "別の name を使うか、先に dx12_delete_entity でどけてくれ");
                // 冪等: 素体は作り直さない（彫った形を失わない）。見た目設定だけ更新する。
                if (params.contains("uvScale"))
                    sc->uvScale = McpFloatParam(params, "uvScale", sc->uvScale, 0.01f, 1000.0f);
                if (params.contains("collision")) sc->collision = params["collision"].get<bool>();
                DirectX::XMFLOAT3 col{};
                if (McpTryVec3(params, "color", col)) sc->color = {col.x, col.y, col.z, 1.0f};
                DirectX::XMFLOAT3 pos{};
                if (McpTryVec3(params, "position", pos) && reg.all_of<Transform>(existing))
                    reg.get<Transform>(existing).position = pos;
                sc->MarkVisualDirty();
                resp["ok"] = true;
                resp["result"] = {
                    {"entityId", static_cast<u32>(existing)}, {"name", name}, {"created", false},
                    {"vertexCount", sc->_data ? sc->_data->VertexCount() : size_t{0}},
                    {"sceneGeneration", m_sceneGeneration},
                    {"note", "同名のスカルプトがあったので素体は作り直さず設定だけ更新した"
                             "(彫った形を消さないため)。別物が欲しいときは name を変えてくれ。"}};
            }
            else
            {
                DirectX::XMFLOAT3 pos{0.0f, 0.0f, 0.0f};
                McpTryVec3(params, "position", pos);
                McpPendingProcCreate pending;   // 外側の req(JSON) を隠さないよう別名(C4456)
                pending.kind         = McpPendingProcCreate::Kind::Sculpt;
                pending.name         = name;
                pending.position     = pos;
                pending.primitive    = prim;
                pending.subdivisions = static_cast<u32>(subdiv);
                pending.size         = size;
                pending.mcp          = deferred;
                m_editorCtx->mcpProcCreates.push_back(std::move(pending));
                isDeferred = true;
            }
        });

    McpDefine("sculpt_make_editable", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot convert a model while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            auto& reg = m_scene->GetRegistry();
            const auto src = ResolveMcpEntity(*m_scene, params);
            if (!reg.all_of<MeshRenderer>(src))
                throw McpError(McpErr::InvalidParam, "entity has no MeshRenderer",
                               "彫れるのはメッシュを持つエンティティだけや");
            if (reg.all_of<SculptMesh>(src))
                throw McpError(McpErr::InvalidParam, "this entity is already a sculpt mesh",
                               "そのまま dx12_sculpt_brush で彫れる");

            const std::string srcName = reg.all_of<NameTag>(src) ? reg.get<NameTag>(src).name
                                                                 : std::string("Model");
            const std::string outName = params.value("name", srcName + "_Sculpt");

            // 冪等: 同名の変換結果が既にあればそれを返す（撃ち直しで増殖しない）。
            for (auto [ent, tag] : reg.view<const NameTag>().each())
            {
                if (tag.name != outName || !reg.all_of<SculptMesh>(ent)) continue;
                resp["ok"] = true;
                resp["result"] = {{"entityId", static_cast<u32>(ent)}, {"name", outName},
                                  {"created", false}, {"sourceEntityId", static_cast<u32>(src)},
                                  {"sceneGeneration", m_sceneGeneration},
                                  {"note", "同名の変換結果が既にあったのでそれを返した"}};
                break;
            }
            if (!resp.contains("result"))
            {
                McpPendingProcCreate pending;   // 外側の req(JSON) を隠さないよう別名(C4456)
                pending.kind   = McpPendingProcCreate::Kind::SculptFromEntity;
                pending.name   = outName;
                pending.source = src;
                pending.mcp    = deferred;
                m_editorCtx->mcpProcCreates.push_back(std::move(pending));
                isDeferred = true;
            }
        });

    McpDefine("sculpt_brush", "brush:string,direction:vec3,entity:int,falloff:number,grabDelta:vec3,"
              "localPosition:vec3,name:string,noiseFrequency:number,noiseOctaves:int,"
              "noiseRidged:number,position:vec3,radius:number,seed:int,strength:number,"
              "symmetryX:bool,symmetryY:bool,symmetryZ:bool", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot sculpt while Playing",
                               "先に dx12_stop で Editor へ戻してくれ");
            using namespace DirectX;
            auto& reg = m_scene->GetRegistry();
            const auto e = ResolveMcpComponentEntity<SculptMesh>(
                *m_scene, params, "sculptMesh",
                "dx12_sculpt_create で素体を作るか、dx12_sculpt_make_editable でモデルを変換してくれ");
            SculptMesh& sc = reg.get<SculptMesh>(e);
            if (!sc._data || !sc._data->IsValid())
                throw McpError(McpErr::Internal, "sculpt mesh has no valid vertex data");
            SculptMeshData& md = *sc._data;

            static const std::vector<std::string> kSculptBrushes{
                "draw", "pull", "push", "smooth", "flatten", "pinch", "noise", "grab"};
            const int bi = McpEnumParam(params, "brush", kSculptBrushes, 0,
                "draw=法線方向に盛る / pull,push=指定方向へ引く・押す / smooth=ならす / "
                "flatten=平らに / pinch=つまむ / noise=岩肌 / grab=掴んで動かす(grabDelta 必須)");

            SculptBrushParams bp;
            bp.type     = static_cast<SculptBrushType>(bi);
            bp.radius   = McpFloatParam(params, "radius",   0.5f, 0.0001f, 10000.0f);
            bp.strength = McpFloatParam(params, "strength", 0.2f, 0.0f, 10000.0f);
            bp.falloff  = McpFloatParam(params, "falloff",  0.5f, 0.0f, 1.0f);
            bp.mirrorX  = params.value("symmetryX", false);
            bp.mirrorY  = params.value("symmetryY", false);
            bp.mirrorZ  = params.value("symmetryZ", false);
            bp.noiseFrequency = McpFloatParam(params, "noiseFrequency", bp.noiseFrequency, 0.0001f, 100.0f);
            bp.noiseOctaves   = McpIntParam  (params, "noiseOctaves",   bp.noiseOctaves,   1, 8);
            bp.noiseRidged    = McpFloatParam(params, "noiseRidged",    bp.noiseRidged,    0.0f, 1.0f);
            bp.noiseSeed      = static_cast<u32>(McpIntParam(params, "seed", 1337, 0, 2000000000));

            // ワールド座標 → メッシュのローカル空間（ブラシはローカルで動く）
            const XMMATRIX world = ComputeWorldMatrix(reg, e);
            XMVECTOR det = XMVectorZero();
            const XMMATRIX inv = XMMatrixInverse(&det, world);
            if (std::fabs(XMVectorGetX(det)) < 1e-20f)
                throw McpError(McpErr::InvalidParam, "entity transform is degenerate (scale 0?)",
                               "dx12_set_transform で scale を 0 以外にしてくれ");

            XMFLOAT3 center{0.0f, 0.0f, 0.0f};
            XMFLOAT3 tmp{};
            if (McpTryVec3(params, "localPosition", tmp))
            {
                center = tmp;
            }
            else
            {
                const XMFLOAT3 wp = McpVec3Required(params, "position");
                XMStoreFloat3(&center, XMVector3TransformCoord(XMLoadFloat3(&wp), inv));
            }
            if (McpTryVec3(params, "direction", tmp))
            {
                XMVECTOR d = XMVector3TransformNormal(XMLoadFloat3(&tmp), inv);
                if (XMVectorGetX(XMVector3LengthSq(d)) > 1e-12f) d = XMVector3Normalize(d);
                else                                             d = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                XMStoreFloat3(&bp.direction, d);
            }
            if (McpTryVec3(params, "grabDelta", tmp))
                XMStoreFloat3(&bp.grabDelta, XMVector3TransformNormal(XMLoadFloat3(&tmp), inv));
            if (bp.type == SculptBrushType::Grab
                && std::fabs(bp.grabDelta.x) + std::fabs(bp.grabDelta.y) + std::fabs(bp.grabDelta.z) < 1e-9f)
                throw McpError(McpErr::InvalidParam, "brush 'grab' needs a non-zero grabDelta",
                               "grabDelta:[dx,dy,dz] にワールド空間の移動量を入れてくれ");

            size_t moved = 0;
            // ★Undo に積みながら彫る（地形側と同じ理由）。MarkDirty もこの中で行う。
            SculptPanel::RunUndoableSculptEdit(reg, *m_editorCtx, e, "Sculpt Brush",
                [&](SculptMeshData& m) { moved = m.ApplyBrush(bp, center, 1.0f, false, nullptr); });

            XMFLOAT3 bmin{}, bmax{};
            md.Bounds(bmin, bmax);
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)},
                {"brush", kSculptBrushes[static_cast<size_t>(bi)]},
                {"movedVertices", moved},
                {"localCenter", {center.x, center.y, center.z}},
                {"radius", bp.radius}, {"strength", bp.strength},
                {"vertexCount", md.VertexCount()}, {"triangleCount", md.TriangleCount()},
                {"localBounds", {{"min", {bmin.x, bmin.y, bmin.z}}, {"max", {bmax.x, bmax.y, bmax.z}}}},
                {"note", "★相対操作（撃つたびに彫れる）。radius/strength は【メッシュのローカル単位】"
                         "＝Transform の scale が掛かる前の大きさ。当てる場所は dx12_pick / "
                         "dx12_raycast_precise の worldPos をそのまま position に渡すのが確実。"}};
        });
}



} // namespace dx12e
