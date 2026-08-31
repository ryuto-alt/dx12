// ===========================================================================
// MCP: カメラ / 空間クエリ / アセット入出力 / ピッキング
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- カメラ / 空間クエリ / アセット入出力 / ピッキング ----
void Application::RegisterMcpAssetMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    McpDefine("get_editor_camera", "targetDistance:number", DX12E_MCP_HANDLER
        {
            // シーンビューを描いているカメラ(Editor=フライカメラ / Playing=ゲームカメラ)の状態。
            const auto pos = m_camera->GetPosition();
            const auto fwd = m_camera->GetForward();
            // ★target を返す（#20-5）。カメラは yaw/pitch しか持たないので target は
            //   `position + forward * targetDistance` で再構成する。
            //   これを dx12_set_editor_camera {position, target} へそのまま渡すと
            //   **同じ yaw/pitch に戻る**（LookAt が逆算する値が一致する）＝読み返し検証ができる。
            const float td = params.contains("targetDistance")
                           ? McpFloatParam(params, "targetDistance", 10.0f, 0.001f, 100000.0f) : 10.0f;
            resp["ok"] = true;
            resp["result"] = {
                {"position", {pos.x, pos.y, pos.z}},
                {"forward",  {fwd.x, fwd.y, fwd.z}},
                {"target",   {pos.x + fwd.x * td, pos.y + fwd.y * td, pos.z + fwd.z * td}},
                {"targetDistance", td},
                {"yawDeg",   DirectX::XMConvertToDegrees(m_camera->GetYaw())},
                {"pitchDeg", DirectX::XMConvertToDegrees(m_camera->GetPitch())},
                {"fovYDeg",  DirectX::XMConvertToDegrees(m_camera->GetFovY())},
                {"aspect",   m_camera->GetAspect()},
                {"nearZ",    m_camera->GetNearZ()},
                {"farZ",     m_camera->GetFarZ()},
                {"orthographic", m_camera->IsOrthographic()},
                {"overridden", m_mcpCameraOverride},
                {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
            };
        });

    McpDefine("set_editor_camera", "pitchDeg:any,position:any,release:bool,target:any,yawDeg:any",
              DX12E_MCP_HANDLER
        {
            // シーンビューのカメラを任意視点へ(focus_camera より自由。俯瞰や引きの構図撮影用)。
            //
            // ★Play 中も使える（#20-6）。Playing 中の m_camera はアクティブな CameraComponent と
            //   毎フレーム同期されるので、そのままでは次のフレームで上書きされてしまう。
            //   そこで **MCP カメラ上書き** を立てて同期を止め、指定した視点を保持する。
            //   `release: true` で上書きを解除するとゲームカメラが再び主導権を持つ。
            //   Play/Stop の遷移でも自動的に解除される（撮影用の一時状態を持ち越さない）。
            if (params.value("release", false))
            {
                m_mcpCameraOverride = false;
                if (m_engineMode == EngineMode::Playing) SyncActiveCameraToGlobal();
                const auto rpos = m_camera->GetPosition();
                const auto rfwd = m_camera->GetForward();
                resp["ok"] = true;
                resp["result"] = {{"released", true},
                                  {"overridden", false},
                                  {"position", {rpos.x, rpos.y, rpos.z}},
                                  {"forward",  {rfwd.x, rfwd.y, rfwd.z}}};
                return;
            }
            if (m_engineMode == EngineMode::Playing)
                m_mcpCameraOverride = true;   // ゲームカメラの毎フレーム同期を止める
            DirectX::XMFLOAT3 pos = m_camera->GetPosition();
            if (params.contains("position"))
            {
                const auto p = params["position"].get<std::vector<float>>();
                if (p.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
                pos = { p[0], p[1], p[2] };
                m_camera->SetPosition(pos);
            }
            if (params.contains("target"))
            {
                const auto tp = params["target"].get<std::vector<float>>();
                if (tp.size() != 3) throw McpError(McpErr::InvalidParam, "target must be [x,y,z]");
                m_camera->LookAt(pos, { tp[0], tp[1], tp[2] });   // yaw/pitch を逆算してくれる
            }
            else
            {
                if (params.contains("yawDeg"))
                    m_camera->SetYaw(DirectX::XMConvertToRadians(params["yawDeg"].get<float>()));
                if (params.contains("pitchDeg"))
                {
                    const float pd = std::clamp(params["pitchDeg"].get<float>(), -89.0f, 89.0f);
                    m_camera->SetPitch(DirectX::XMConvertToRadians(pd));
                }
            }
            const auto npos = m_camera->GetPosition();
            const auto nfwd = m_camera->GetForward();
            resp["ok"] = true;
            resp["result"] = {
                {"position", {npos.x, npos.y, npos.z}},
                {"forward",  {nfwd.x, nfwd.y, nfwd.z}},
                {"yawDeg",   DirectX::XMConvertToDegrees(m_camera->GetYaw())},
                {"pitchDeg", DirectX::XMConvertToDegrees(m_camera->GetPitch())},
                {"overridden", m_mcpCameraOverride},
                {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                {"note", m_mcpCameraOverride
                             ? "Play 中のゲームカメラ同期を止めて視点を固定した。"
                               "dx12_set_editor_camera {\"release\":true} で解除（Stop でも自動解除）"
                             : ""},
            };
        });

    McpDefine("get_bounds", "entity:int,includeChildren:bool,name:string,perSubmesh:bool", DX12E_MCP_HANDLER
        {
            // ワールド AABB。AI が「どこに何を置くか」を数値で決めるための基礎情報。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            DirectX::XMFLOAT3 mn, mx;
            bool hasMesh = false;
            if (!McpWorldAabb(reg, e, mn, mx, hasMesh))
                throw McpError(McpErr::NotFound, "entity has no Transform");
            if (params.value("includeChildren", false))
            {
                auto view = reg.view<const Transform>();
                for (auto c : view)
                {
                    if (c == e || !McpIsDescendantOf(reg, c, e)) continue;
                    DirectX::XMFLOAT3 cmn, cmx;
                    bool chm = false;
                    if (!McpWorldAabb(reg, c, cmn, cmx, chm)) continue;
                    mn = { std::min(mn.x, cmn.x), std::min(mn.y, cmn.y), std::min(mn.z, cmn.z) };
                    mx = { std::max(mx.x, cmx.x), std::max(mx.y, cmx.y), std::max(mx.z, cmx.z) };
                    hasMesh = hasMesh || chm;
                }
            }
            resp["ok"] = true;
            resp["result"] = {
                {"entityId", static_cast<u32>(e)},
                {"min", {mn.x, mn.y, mn.z}}, {"max", {mx.x, mx.y, mx.z}},
                {"center", {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f}},
                {"size", {mx.x - mn.x, mx.y - mn.y, mx.z - mn.z}},
                {"hasMesh", hasMesh},
            };

            // ★サブメッシュ内訳（perSubmesh:true）。
            //   「モデルの一部だけ変な位置に飛んでいる。どの部品か」を 1 回で特定するための口。
            //   index は **dx12_pick が返す submeshIndex と同じ番号**（どちらも
            //   MeshRenderer.meshes の並び）なので、そのまま突き合わせられる。
            //   glTF/FBX の JSON 内の並びとは一致しない（ModelLoader がノード単位に展開するため）
            //   ＝だからこそ name / materialName を一緒に返す。
            if (params.value("perSubmesh", false))
            {
                using namespace DirectX;
                const auto* mr = reg.try_get<MeshRenderer>(e);
                if (!mr) throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
                const XMMATRIX world = ComputeWorldMatrix(reg, e);
                json arr = json::array();
                f32 biggest = 0.0f;
                int biggestIdx = -1;
                for (size_t i = 0; i < mr->meshes.size(); ++i)
                {
                    const Mesh* m = mr->meshes[i];
                    if (!m) { arr.push_back({{"index", static_cast<u32>(i)}, {"missing", true}}); continue; }
                    const XMFLOAT3 lmn = m->GetAABBMin(), lmx = m->GetAABBMax();
                    // ノードアニメ付きモデルは meshNodeTransforms がサブメッシュごとの姿勢を持つ
                    XMMATRIX node = XMMatrixIdentity();
                    if (i < mr->meshNodeTransforms.size())
                        node = XMLoadFloat4x4(&mr->meshNodeTransforms[i]);
                    const XMMATRIX full = node * world;
                    XMFLOAT3 wmn{0, 0, 0}, wmx{0, 0, 0};
                    for (int c = 0; c < 8; ++c)
                    {
                        const XMVECTOR p = XMVector3Transform(
                            XMVectorSet((c & 1) ? lmx.x : lmn.x,
                                        (c & 2) ? lmx.y : lmn.y,
                                        (c & 4) ? lmx.z : lmn.z, 1.0f), full);
                        XMFLOAT3 q; XMStoreFloat3(&q, p);
                        if (c == 0) { wmn = q; wmx = q; }
                        else
                        {
                            wmn = { (std::min)(wmn.x, q.x), (std::min)(wmn.y, q.y), (std::min)(wmn.z, q.z) };
                            wmx = { (std::max)(wmx.x, q.x), (std::max)(wmx.y, q.y), (std::max)(wmx.z, q.z) };
                        }
                    }
                    const XMFLOAT3 wsize{ wmx.x - wmn.x, wmx.y - wmn.y, wmx.z - wmn.z };
                    // 「原点からどれだけ離れているか」= 飛んでいる部品を見つける物差し
                    const XMFLOAT3 wc{ (wmn.x + wmx.x) * 0.5f, (wmn.y + wmx.y) * 0.5f, (wmn.z + wmx.z) * 0.5f };
                    const f32 span = (std::max)((std::max)(wsize.x, wsize.y), wsize.z);
                    if (span > biggest) { biggest = span; biggestIdx = static_cast<int>(i); }
                    arr.push_back({
                        {"index", static_cast<u32>(i)},
                        {"name", m->GetName()},
                        {"materialName", m->GetMaterialName()},
                        {"triangles", m->GetIndexCount() / 3},
                        {"localMin", {lmn.x, lmn.y, lmn.z}}, {"localMax", {lmx.x, lmx.y, lmx.z}},
                        {"localSize", {lmx.x - lmn.x, lmx.y - lmn.y, lmx.z - lmn.z}},
                        {"worldMin", {wmn.x, wmn.y, wmn.z}}, {"worldMax", {wmx.x, wmx.y, wmx.z}},
                        {"worldCenter", {wc.x, wc.y, wc.z}},
                        {"worldSize", {wsize.x, wsize.y, wsize.z}},
                    });
                }
                resp["result"]["submeshes"]     = std::move(arr);
                resp["result"]["submeshCount"]  = static_cast<u32>(mr->meshes.size());
                resp["result"]["largestSubmesh"] = biggestIdx;
                resp["result"]["submeshNote"] =
                    "index は dx12_pick の submeshIndex と同じ並び。worldSize/worldCenter が"
                    "他と桁違いの部品が『飛んでいる』もの。name/materialName は DCC 側の呼び名";
            }
        });

    McpDefine("look_at", "entity:int,name:string,target:any,targetEntity:any,targetName:any,upright:bool", DX12E_MCP_HANDLER
        {
            // エンティティを目標(座標 or 別エンティティ)へ向ける。+Z が正面の想定(rotation Euler を書く)。
            // 注: rotation はローカル値なので、親が回転していると厳密なワールド向きからずれる。
            using namespace DirectX;
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Transform>(e)) throw McpError(McpErr::NotFound, "entity has no Transform");
            XMFLOAT3 tgt;
            if (params.contains("target"))
            {
                const auto tp = params["target"].get<std::vector<float>>();
                if (tp.size() != 3) throw McpError(McpErr::InvalidParam, "target must be [x,y,z]");
                tgt = { tp[0], tp[1], tp[2] };
            }
            else
            {
                json tref;
                if (params.contains("targetEntity"))    tref["entity"] = params["targetEntity"];
                else if (params.contains("targetName")) tref["name"]   = params["targetName"];
                else throw McpError(McpErr::InvalidParam,
                    "need 'target' [x,y,z] or 'targetEntity'(id) / 'targetName'");
                const auto te = ResolveMcpEntity(*m_scene, tref);
                if (te == e) throw McpError(McpErr::InvalidParam, "cannot look at itself");
                XMFLOAT4X4 twf;
                XMStoreFloat4x4(&twf, ComputeWorldMatrix(reg, te));
                tgt = { twf._41, twf._42, twf._43 };
            }
            XMFLOAT4X4 wf;
            XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, e));
            const XMFLOAT3 dir{ tgt.x - wf._41, tgt.y - wf._42, tgt.z - wf._43 };
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (len < 1e-6f) throw McpError(McpErr::InvalidParam, "target coincides with entity position");
            const float yawDeg = XMConvertToDegrees(std::atan2(dir.x, dir.z));
            float pitchDeg = 0.0f;
            if (!params.value("upright", false))
                pitchDeg = XMConvertToDegrees(-std::asin(std::clamp(dir.y / len, -1.0f, 1.0f)));
            auto& t = reg.get<Transform>(e);
            t.rotation = { pitchDeg, yawDeg, 0.0f };
            t.useQuaternion = false;
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)},
                              {"rotation", {pitchDeg, yawDeg, 0.0f}},
                              {"target", {tgt.x, tgt.y, tgt.z}}};
        });

    McpDefine("snap_to_ground", "entity:int,name:string,offset:number,precise:bool", DX12E_MCP_HANDLER
        {
            // Editor 中でも使える AABB ベースの接地。XZ が重なる他メッシュの天面のうち
            // 自分の天面以下で最も高いものに底面を合わせる。無ければ y=0 平面へ。
            // (Playing 中の物理レイキャストは dx12_raycast を使う)
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Transform>(e)) throw McpError(McpErr::NotFound, "entity has no Transform");
            DirectX::XMFLOAT3 mn, mx;
            bool hasMesh = false;
            McpWorldAabb(reg, e, mn, mx, hasMesh);
            {   // 子も含めた AABB(モデルのルートが empty のことがあるため)
                auto view = reg.view<const Transform>();
                for (auto c : view)
                {
                    if (c == e || !McpIsDescendantOf(reg, c, e)) continue;
                    DirectX::XMFLOAT3 cmn, cmx;
                    bool chm = false;
                    if (!McpWorldAabb(reg, c, cmn, cmx, chm) || !chm) continue;
                    mn = { std::min(mn.x, cmn.x), std::min(mn.y, cmn.y), std::min(mn.z, cmn.z) };
                    mx = { std::max(mx.x, cmx.x), std::max(mx.y, cmx.y), std::max(mx.z, cmx.z) };
                }
            }
            const float offset = params.value("offset", 0.0f);
            float groundY = 0.0f;
            entt::entity groundEnt = entt::null;
            const char* snapMethod = "aabb";

            // ① 三角形精密レイキャストで真下の「実際の面」を取る。
            //    AABB の天面だけを見ていた旧実装は、地形のように 1 メッシュが広く起伏する相手だと
            //    山頂の高さに吸い付いてしまい「地形の上に浮く」。エディタのピッキングと同じ
            //    ScenePick を通すので、斜面・階段・彫った岩にもちゃんと乗る。
            if (params.value("precise", true))
            {
                const DirectX::XMFLOAT3 rayO{ (mn.x + mx.x) * 0.5f, mx.y + 0.05f, (mn.z + mx.z) * 0.5f };
                const DirectX::XMFLOAT3 rayD{ 0.0f, -1.0f, 0.0f };
                ScenePickOptions popt;
                popt.includeNonMesh  = false;   // アイコン/スプライトは床にしない
                popt.trianglePrecise = true;
                popt.maxCandidates   = 256;
                for (const ScenePickHit& h : RaycastSceneRay(reg, &GetDrawItems(), rayO, rayD, 0.0f, popt))
                {
                    // 自分自身・子孫・祖先は床にしない（距離昇順なので最初の他人が直下の面）
                    if (h.entity == e || McpIsDescendantOf(reg, h.entity, e)
                        || McpIsDescendantOf(reg, e, h.entity)) continue;
                    groundY    = h.worldPos.y;
                    groundEnt  = h.entity;
                    snapMethod = "raycast";
                    break;
                }
            }

            if (groundEnt == entt::null)
            {
                // ② フォールバック: XZ が重なる他メッシュの天面（従来の AABB 判定）。
                //    真下に三角形が無い（穴の上・メッシュの外）ときはこっちが働く。
                auto view = reg.view<const Transform, const MeshRenderer>();
                for (auto o : view)
                {
                    if (o == e || McpIsDescendantOf(reg, o, e) || McpIsDescendantOf(reg, e, o)) continue;
                    DirectX::XMFLOAT3 omn, omx;
                    bool ohm = false;
                    if (!McpWorldAabb(reg, o, omn, omx, ohm) || !ohm) continue;
                    if (omx.x < mn.x || omn.x > mx.x || omx.z < mn.z || omn.z > mx.z) continue;  // XZ 非重複
                    if (omx.y > mx.y + 1e-3f) continue;   // 完全に自分より上の床は無視
                    if (groundEnt == entt::null || omx.y > groundY) { groundY = omx.y; groundEnt = o; }
                }
            }
            const float delta = (groundY + offset) - mn.y;
            auto& t = reg.get<Transform>(e);
            t.position.y += delta;   // 親に回転/スケールがあると厳密でない(ルート配置想定)
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"groundY", groundY},
                              {"movedBy", delta}, {"method", snapMethod},
                              {"position", {t.position.x, t.position.y, t.position.z}}};
            if (groundEnt != entt::null)
                resp["result"]["groundEntityId"] = static_cast<u32>(groundEnt);
            else
                resp["result"]["note"] = "no ground mesh found; snapped to y=0 plane";
        });

    McpDefine("get_hierarchy", "", DX12E_MCP_HANDLER
        {
            // シーン全体の親子ツリー。list_entities のフラット一覧と違い構造が分かる。
            auto& reg = m_scene->GetRegistry();
            std::unordered_map<entt::entity, std::vector<entt::entity>> children;
            std::vector<entt::entity> roots;
            size_t total = 0;
            auto view = reg.view<const NameTag>();
            for (auto e : view)
            {
                ++total;
                entt::entity parent = entt::null;
                if (reg.all_of<Transform>(e)) parent = reg.get<Transform>(e).parent;
                if (parent != entt::null && reg.valid(parent)) children[parent].push_back(e);
                else roots.push_back(e);
            }
            std::function<json(entt::entity, int)> build = [&](entt::entity e, int depth) -> json
            {
                json n{{"entityId", static_cast<u32>(e)},
                       {"name", reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string()}};
                if (depth < 64)
                {
                    auto it = children.find(e);
                    if (it != children.end())
                    {
                        json kids = json::array();
                        for (auto c : it->second) kids.push_back(build(c, depth + 1));
                        n["children"] = std::move(kids);
                    }
                }
                return n;
            };
            json rootsJson = json::array();
            for (auto r : roots) rootsJson.push_back(build(r, 0));
            resp["ok"] = true;
            resp["result"] = {{"roots", std::move(rootsJson)}, {"count", total},
                              {"sceneGeneration", m_sceneGeneration}};
        });

    McpDefine("import_asset", "destPath:string,overwrite:bool,sourcePath:string", DX12E_MCP_HANDLER
        {
            // 外部ファイル/フォルダを assets へコピーする(唯一 assets 外を「読む」ツール。
            // 書き先は assets 限定)。/asset コマンド等で落とした素材の取り込みに使う。
            const std::string src = params.value("sourcePath", std::string());
            if (src.empty()) throw McpError(McpErr::InvalidParam, "missing 'sourcePath'");
            const std::string destRel =
                ValidateMcpAssetRelPath(params.value("destPath", std::string()), "destPath");
            const bool overwrite = params.value("overwrite", false);
            const fs::path srcPath(src);
            if (!fs::exists(srcPath)) throw McpError(McpErr::NotFound, "source not found: " + src);
            const fs::path assetsRoot(PathResolver::AssetsDir());
            fs::path dest = assetsRoot / destRel;
            json imported = json::array();
            if (fs::is_directory(srcPath))
            {
                if (fs::exists(dest) && !overwrite)
                    throw McpError(McpErr::InvalidParam,
                        "destination exists (overwrite:true で上書き): " + destRel);
                fs::create_directories(dest);
                fs::copy(srcPath, dest,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                for (const auto& de : fs::recursive_directory_iterator(dest))
                    if (de.is_regular_file())
                        imported.push_back(fs::relative(de.path(), assetsRoot).generic_string());
            }
            else
            {
                // destRel が既存ディレクトリ or 末尾 '/' ならファイル名を引き継ぐ
                if ((fs::exists(dest) && fs::is_directory(dest)) || destRel.back() == '/')
                    dest /= srcPath.filename();
                if (fs::exists(dest) && !overwrite)
                    throw McpError(McpErr::InvalidParam,
                        "destination exists (overwrite:true で上書き): "
                        + fs::relative(dest, assetsRoot).generic_string());
                fs::create_directories(dest.parent_path());
                fs::copy_file(srcPath, dest, fs::copy_options::overwrite_existing);
                imported.push_back(fs::relative(dest, assetsRoot).generic_string());
            }
            resp["ok"] = true;
            resp["result"] = {{"imported", imported}, {"count", imported.size()}};
            {
                std::string ext = srcPath.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".gltf")
                    resp["result"]["note"] =
                        ".gltf は同階層の .bin / テクスチャを参照する。ロードに失敗したらフォルダごと import する";
            }
        });

    McpDefine("asset_info", "path:string", DX12E_MCP_HANDLER
        {
            // アセットのメタ情報。モデルは Assimp、テクスチャは DirectXTex で GPU を使わず読む。
            const std::string rel = ValidateMcpAssetRelPath(params.value("path", std::string()));
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            if (!fs::exists(full) || !fs::is_regular_file(full))
                throw McpError(McpErr::NotFound, "asset not found: " + rel);
            std::string ext = full.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            json result{{"path", rel}, {"fileSizeBytes", static_cast<u64>(fs::file_size(full))}};
            if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj")
            {
                result["type"] = "model";
                const auto info = ModelLoader::Probe(full);
                if (!info.ok) throw McpError(McpErr::Internal, "model probe failed: " + info.error);
                result["meshCount"]      = info.meshCount;
                result["materialCount"]  = info.materialCount;
                result["totalVertices"]  = info.totalVertices;
                result["totalFaces"]     = info.totalFaces;
                result["boneCount"]      = info.boneCount;
                result["hasSkeleton"]    = info.hasSkeleton;
                json anims = json::array();
                for (const auto& a : info.animations)
                    anims.push_back({{"name", a.name}, {"durationSec", a.durationSec}});
                result["animations"] = std::move(anims);
                result["aabbMin"] = {info.aabbMin[0], info.aabbMin[1], info.aabbMin[2]};
                result["aabbMax"] = {info.aabbMax[0], info.aabbMax[1], info.aabbMax[2]};
                result["aabbNote"] = "ノード変換込みのワールド AABB(スケール1で spawn した時の実サイズ)";
            }
            else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" ||
                     ext == ".tga" || ext == ".bmp" || ext == ".hdr")
            {
                result["type"] = "texture";
                const auto info = TextureLoader::Probe(full.wstring());
                if (!info.ok) throw McpError(McpErr::Internal, "texture probe failed: " + info.error);
                result["width"]     = info.width;
                result["height"]    = info.height;
                result["mipLevels"] = info.mipLevels;
                result["arraySize"] = info.arraySize;
                result["format"]    = info.format;
                result["isCubemap"] = info.isCubemap;
            }
            else
            {
                result["type"] = (ext == ".lua")    ? "script"
                               : (ext == ".hlsl")   ? "shader"
                               : (ext == ".prefab") ? "prefab"
                               : (ext == ".json")   ? "scene"
                               : (ext == ".wav" || ext == ".mp3" || ext == ".ogg") ? "audio"
                               : "other";
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    McpDefine("reload_assets", "force:bool,path:string", DX12E_MCP_HANDLER
        {
            // ディスク上で書き換わったアセットだけをキャッシュごと読み直す。
            //
            // ★これが無かったとき何が起きていたか: Blender から同じパスへ書き出し直しても、
            //   ResourceManager のテクスチャ/モデルキャッシュが一生ヒットするので絵が変わらない。
            //   結果、見た目を確認するたびにエディタを落として起動し直す(1 往復 20 秒以上)
            //   ハメになっていた。ここがその再起動を消すための口。
            //
            // ★シーンは開き直さない。テクスチャは「同じ Texture オブジェクト・同じ SRV 番号」の
            //   まま中身だけ差し替わり、モデルは実体を作り直したうえで今シーンに置かれている
            //   MeshRenderer の参照を張り替える(= エンティティも Transform も選択状態も残る)。
            //
            // 実ロードは GPU アップロードを伴うので cmdList が有効なフレーム境界へ回す
            // (mcpProcCreates と同じ流儀)。応答は遅延。
            if (!m_editorCtx) throw McpError(McpErr::Internal, "editor context not ready");
            McpPendingAssetReload req;
            req.force = params.value("force", false);
            const std::string rel = params.value("path", std::string());
            if (rel.empty())
            {
                req.prefix   = fs::path(PathResolver::AssetsDir()).lexically_normal().generic_string();
                req.relLabel = "(assets 全体)";
            }
            else
            {
                const std::string v = ValidateMcpAssetRelPath(rel);
                const fs::path full = fs::path(PathResolver::AssetsDir()) / v;
                if (!fs::exists(full))
                    throw McpError(McpErr::NotFound, "asset not found: " + v);
                req.prefix   = full.lexically_normal().generic_string();
                req.relLabel = v;
            }
            req.mcp = deferred;
            m_editorCtx->mcpAssetReloads.push_back(std::move(req));
            isDeferred = true;
        });

    McpDefine("read_texture", "maxSize:int,path:string", DX12E_MCP_HANDLER
        {
            // 任意対応形式(dds/tga 含む)のテクスチャを PNG に変換して絶対パスを返す。
            // Node 側が画像ブロックとして AI に見せる(dx12_view_texture)。
            const std::string rel = ValidateMcpAssetRelPath(params.value("path", std::string()));
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "texture not found: " + rel);
            const int maxSize = params.value("maxSize", 1024);
            if (maxSize < 16 || maxSize > 4096)
                throw McpError(McpErr::InvalidParam, "maxSize must be 16..4096");
            const fs::path outPath = fs::absolute("mcp_texture.png");   // screenshot と同じく CWD へ上書き
            std::string err;
            uint32_t w = 0, h = 0;
            if (!TextureLoader::ConvertToPng(full.wstring(), outPath.wstring(),
                                             static_cast<uint32_t>(maxSize), err, w, h))
                throw McpError(McpErr::Internal, "texture convert failed: " + err);
            resp["ok"] = true;
            resp["result"] = {{"path", outPath.string()}, {"width", w}, {"height", h},
                              {"sourcePath", rel}};
        });

    McpDefine("move_asset", "from:string,overwrite:bool,to:string,updateFiles:bool", DX12E_MCP_HANDLER
        {
            // assets 内のファイル/フォルダの移動・リネーム。参照パスは自動更新しない。
            const std::string fromRel =
                ValidateMcpAssetRelPath(params.value("from", std::string()), "from");
            const std::string toRel =
                ValidateMcpAssetRelPath(params.value("to", std::string()), "to");
            const fs::path assetsRoot(PathResolver::AssetsDir());
            const fs::path fromP = assetsRoot / fromRel;
            const fs::path toP   = assetsRoot / toRel;
            if (!fs::exists(fromP)) throw McpError(McpErr::NotFound, "asset not found: " + fromRel);
            if (fs::exists(toP))
            {
                if (!params.value("overwrite", false))
                    throw McpError(McpErr::InvalidParam,
                        "destination exists (overwrite:true で上書き): " + toRel);
                if (fs::is_directory(toP))
                    throw McpError(McpErr::InvalidParam, "cannot overwrite a directory: " + toRel);
                fs::remove(toP);
            }
            fs::create_directories(toP.parent_path());
            fs::rename(fromP, toP);

            // 開いているシーンの参照を追従させる。切れても実行時にエラーが出ない
            // （音が鳴らない / UI が真っ白 になるだけ）ので、放置すると気付けない。
            // ★書き換えるのは「いま開いているシーン」のメモリ上の値だけ。
            //   保存しないとディスクには載らないし、他のシーンや .prefab は直らない。
            const int updated = SceneSerializer::RewriteAssetPathRefs(*m_scene, fromRel, toRel);
            if (updated > 0 && m_editorCtx) m_editorCtx->undoSystem.MarkEdited();

            // ディスク上の他ファイル（他シーン / .prefab / .dxmat / .animfsm /
            // .spranim / .terrainlayers / .uianim）も直す。アセット同士の参照も
            // 同じ問題を抱えているので同じ経路で扱う。
            // 開いているシーンのファイルは除外する。メモリ側を既に直しており、
            // 保存時にそちらが書かれるので二重に触る必要が無い。
            SceneSerializer::AssetRefFileRewrite files{};
            if (params.value("updateFiles", true))
                files = SceneSerializer::RewriteAssetPathRefsInFiles(
                    PathResolver::AssetsDir(), fromRel, toRel,
                    m_editorCtx ? m_editorCtx->currentScenePath : std::string());

            json changedFiles = json::array();
            for (const auto& f : files.files) changedFiles.push_back(f);
            json failedFiles = json::array();
            for (const auto& f : files.failed) failedFiles.push_back(f);

            resp["ok"] = true;
            resp["result"] = {{"from", fromRel}, {"to", toRel},
                              {"refsUpdated", updated},
                              {"filesChanged", files.filesChanged},
                              {"fileRefsUpdated", files.refsChanged},
                              {"changedFiles", changedFiles},
                              {"failedFiles", failedFiles},
                              {"note", updated > 0
                                  ? "開いているシーンの参照はメモリ上で更新した。dx12_save_scene で保存すること"
                                  : "開いているシーンにこのアセットへの参照は無かった"}};
        });

    McpDefine("delete_asset", "path:string,recursive:bool", DX12E_MCP_HANDLER
        {
            // assets 内のファイル削除。ディレクトリは recursive:true 必須(誤爆防止)。
            const std::string rel = ValidateMcpAssetRelPath(params.value("path", std::string()));
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "asset not found: " + rel);

            // 消す前に「まだ誰が使っているか」を数える。消してから数えても手遅れなので順序が要点。
            // 削除は止めない（消したい理由がある場合を邪魔しない）が、黙って壊すのはやめる。
            std::vector<std::string> who;
            const int refCount = SceneSerializer::CountAssetPathRefs(*m_scene, rel, &who);
            json refWho = json::array();
            for (const auto& w : who) refWho.push_back(w);

            uintmax_t removed = 0;
            bool wasDirectory = false;
            if (fs::is_directory(full))
            {
                wasDirectory = true;
                if (!params.value("recursive", false))
                    throw McpError(McpErr::InvalidParam,
                        "'" + rel + "' is a directory (recursive:true で丸ごと削除)");
                removed = fs::remove_all(full);
            }
            else
            {
                removed = fs::remove(full) ? 1 : 0;
            }
            resp["ok"] = true;
            resp["result"] = {{"deleted", rel}, {"removedCount", removed},
                              {"wasDirectory", wasDirectory},
                              {"stillReferencedBy", refWho},
                              {"stillReferencedCount", refCount},
                              {"note", refCount > 0
                                  ? "★開いているシーンがまだこのアセットを参照している。"
                                    "実行時はエラー無しで「出ない/鳴らない」だけになるので、"
                                    "参照元を直すか dx12_diagnose {only:[\"scene_assets\"]} で確認すること"
                                  : "開いているシーンからの参照は無かった"}};
        });

    // ════════════════════════════════════════════════════════════
    //  精密ピッキング（三角形単位。エディタのクリック選択と同じ実装を共有）
    // ════════════════════════════════════════════════════════════
    McpDefine("pick", "all:bool,includeIcons:bool,maxCandidates:int,maxHits:int,trianglePrecise:bool,"
              "u:number,v:number,x:number,y:number", DX12E_MCP_HANDLER
        {
            if (!m_camera || !m_sceneRT) throw McpError(McpErr::Internal, "renderer not ready");
            const f32 vw = static_cast<f32>(m_sceneRT->GetWidth());
            const f32 vh = static_cast<f32>(m_sceneRT->GetHeight());
            if (!(vw > 1.0f && vh > 1.0f))
                throw McpError(McpErr::Internal, "scene render target has no size");

            f32 sx = 0.0f, sy = 0.0f;
            if (params.contains("x") && params.contains("y"))
            {
                sx = McpFloatParam(params, "x", 0.0f, 0.0f, vw - 1.0f);
                sy = McpFloatParam(params, "y", 0.0f, 0.0f, vh - 1.0f);
            }
            else if (params.contains("u") && params.contains("v"))
            {
                sx = McpFloatParam(params, "u", 0.5f, 0.0f, 1.0f) * (vw - 1.0f);
                sy = McpFloatParam(params, "v", 0.5f, 0.0f, 1.0f) * (vh - 1.0f);
            }
            else
            {
                throw McpError(McpErr::InvalidParam, "need x/y (pixels) or u/v (0..1)",
                    "画面中央なら {u:0.5, v:0.5}。ピクセル指定は dx12_screenshot / "
                    "dx12_project_world_to_screen と同じ左上原点の座標系（今は "
                    + std::to_string(m_sceneRT->GetWidth()) + "x"
                    + std::to_string(m_sceneRT->GetHeight()) + "）");
            }

            ScenePickOptions opt;
            opt.includeNonMesh  = params.value("includeIcons", true);
            opt.trianglePrecise = params.value("trianglePrecise", true);
            opt.maxCandidates   = static_cast<u32>(McpIntParam(params, "maxCandidates", 64, 1, 4096));

            auto& reg = m_scene->GetRegistry();
            const std::vector<ScenePickHit> hits = RaycastScene(
                reg, &GetDrawItems(), *m_camera, 0.0f, 0.0f, vw, vh, sx, sy, opt);

            json result = McpPickHitsJson(reg, hits, params.value("all", false),
                                          McpIntParam(params, "maxHits", 16, 1, 64));
            result["screen"]   = {{"x", sx}, {"y", sy}};
            result["viewport"] = {{"width", m_sceneRT->GetWidth()}, {"height", m_sceneRT->GetHeight()}};
            result["mode"]     = (m_engineMode == EngineMode::Playing) ? "Playing" : "Editor";
            result["renderScale"] = m_renderScale;
            result["note"]     = "シーンビューを描いているカメラ基準(Playing 中はゲームカメラ)。"
                                 "座標系は dx12_screenshot / dx12_project_world_to_screen と同じ"
                                 "（= レンダー解像度。renderScale<1 なら表示ピクセルより小さい）。"
                                 "エディタの左クリック選択と同じ RaycastScene を通すので結果は一致する。"
                                 "スキンドメッシュはバインドポーズの AABB 止まり。";
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    McpDefine("raycast_precise", "all:bool,direction:vec3,maxCandidates:int,maxDistance:number,maxHits:int,origin:vec3,"
              "trianglePrecise:bool", DX12E_MCP_HANDLER
        {
            const DirectX::XMFLOAT3 o = McpVec3Required(params, "origin");
            const DirectX::XMFLOAT3 d = McpVec3Required(params, "direction");
            const f32 maxDist = McpFloatParam(params, "maxDistance", 1000.0f, 0.0f, 1.0e7f);

            ScenePickOptions opt;
            opt.includeNonMesh  = false;   // ワールドレイ版はアイコン/ビルボードを見ない
            opt.trianglePrecise = params.value("trianglePrecise", true);
            opt.maxCandidates   = static_cast<u32>(McpIntParam(params, "maxCandidates", 256, 1, 4096));

            auto& reg = m_scene->GetRegistry();
            const std::vector<ScenePickHit> hits =
                RaycastSceneRay(reg, &GetDrawItems(), o, d, maxDist, opt);

            json result = McpPickHitsJson(reg, hits, params.value("all", false),
                                          McpIntParam(params, "maxHits", 16, 1, 64));
            result["origin"]      = {o.x, o.y, o.z};
            result["direction"]   = {d.x, d.y, d.z};
            result["maxDistance"] = maxDist;
            result["note"] = "★描画メッシュの三角形基準（dx12_raycast は物理コライダー基準で Playing 中限定）。"
                             "こっちは Editor でも動き、地形/スカルプトの実際の表面に当たる。"
                             "スキンドメッシュだけはバインドポーズの AABB 止まり。";
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });
}




// ---------------------------------------------------------------------------
// dx12_reload_assets の実処理(フレーム境界。ApplicationRender.cpp から 1 度だけ呼ぶ)
// ---------------------------------------------------------------------------
// ★ここは「参照の張り替え」まで含めて 1 セット。ResourceManager が CachedModel を
//   作り直すと、そのモデルを使っている MeshRenderer.meshes / .materials と
//   Mesh::m_material、そして RaytracingScene の BLAS キャッシュ(キーが Mesh*)が
//   まとめて宙に浮く。**同じ関数の中で全部張り替え切ること**(描画は後段なので間に合う)。
void Application::ProcessMcpAssetReloads(ID3D12GraphicsCommandList* cmdList)
{
    using json = nlohmann::json;
    if (!m_editorCtx || m_editorCtx->mcpAssetReloads.empty()) return;
    auto reqs = std::move(m_editorCtx->mcpAssetReloads);
    m_editorCtx->mcpAssetReloads.clear();
    if (!cmdList || !m_resourceManager || !m_scene)
    {
        for (const auto& req : reqs)
            FailMcp(m_mcpBridge.get(), req.mcp, McpErr::Internal, "renderer not ready");
        return;
    }

    const std::string assetsDir = PathResolver::AssetsDir();
    auto toRel = [&](const std::string& abs) -> std::string
    {
        const std::string a = std::filesystem::path(abs).lexically_normal().generic_string();
        const std::string root = std::filesystem::path(assetsDir).lexically_normal().generic_string();
        if (a.size() > root.size() && _strnicmp(a.c_str(), root.c_str(), root.size()) == 0)
            return a.substr(root.size());
        return a;
    };

    for (const auto& req : reqs)
    {
        const AssetReloadResult r =
            m_resourceManager->ReloadChangedAssets(cmdList, req.prefix, req.force);

        // ---- モデルを読み直したなら、今シーンの MeshRenderer を新しい実体へ張り替える ----
        int reboundEntities = 0;
        json warnings = json::array();
        if (!r.models.empty())
        {
            std::unordered_map<std::string, const CachedModel*> byKey;
            for (const auto& mp : r.models)
            {
                const std::string k = ResourceManager::NormalizeModelKey(mp);
                byKey[k] = m_resourceManager->FindModel(k);
            }
            auto& reg = m_scene->GetRegistry();
            for (auto [e, mr] : reg.view<MeshRenderer>().each())
            {
                if (mr.modelPath.empty()) continue;
                // modelPath は「assets 相対」で保存されるが、D&D 配置直後は絶対パスのこともある。
                // GetOrLoadModel と同じ正規化を両方の綴りで試す(片方でも当たれば同じモデル)。
                const CachedModel* cm = nullptr;
                {
                    auto it = byKey.find(ResourceManager::NormalizeModelKey(mr.modelPath));
                    if (it == byKey.end())
                        it = byKey.find(ResourceManager::NormalizeModelKey(assetsDir + mr.modelPath));
                    if (it != byKey.end()) cm = it->second;
                }
                if (!cm) continue;

                // ★ヒットしたら必ず張り替える。「アニメ付きだから見送る」は
                //   死んだポインタを残す＝次の描画で落ちるので絶対にやらない。
                const size_t before = mr.meshes.size();
                mr.meshes.clear();
                mr.materials.clear();
                for (const auto& m : cm->meshes)    mr.meshes.push_back(m.get());
                for (const auto& m : cm->materials) mr.materials.push_back(m.get());
                if (!mr.meshNodeTransforms.empty() || cm->nodeGraph)
                {
                    DirectX::XMFLOAT4X4 ident;
                    DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
                    mr.meshNodeTransforms.assign(mr.meshes.size(), ident);
                }
                ++reboundEntities;
                if (before != mr.meshes.size()
                    && (reg.all_of<SkeletalAnimation>(e) || reg.all_of<NodeAnimationComp>(e)))
                {
                    warnings.push_back(
                        "サブメッシュ数が " + std::to_string(before) + " -> "
                        + std::to_string(mr.meshes.size()) + " に変わりました(" + mr.modelPath
                        + ")。アニメーションの割り当ては作り直さないので、動きがおかしければ "
                          "dx12_open_scene でシーンを開き直すこと");
                }
            }
            // BLAS キャッシュのキーは Mesh*。解放したアドレスへ新しいメッシュが載ると
            // 「ラスタは正しいのにレイトレの影/反射だけ古い形」になるので全部捨てる。
            if (m_rtScene) m_rtScene->Invalidate();
        }

        // ---- テクスチャを読み直したなら、マテリアル上書きの SRV ブロックも作り直す ----
        // (ModelLoader が組む Material::srvBlockIndex 側は ResourceManager が張り直し済み)
        if (!r.textures.empty() && m_srvHeap)
        {
            for (auto& kv : m_materialOverrideSrvCache)
                if (kv.second.blockStart != 0xFFFFFFFF) m_srvHeap->FreeBlock(kv.second.blockStart, 3);
            m_materialOverrideSrvCache.clear();
        }

        json textures = json::array();
        for (const auto& t : r.textures) textures.push_back(toRel(t));
        json models = json::array();
        for (const auto& m : r.models) models.push_back(toRel(m));
        json skipped = json::array();
        for (const auto& sk : r.skipped) skipped.push_back(sk);

        const size_t total = r.textures.size() + r.models.size();
        Logger::Info("アセット再読込: {} (テクスチャ{}件 / モデル{}件 / 張り替え{}体)",
                     req.relLabel, r.textures.size(), r.models.size(), reboundEntities);

        CompleteMcp(m_mcpBridge.get(), req.mcp,
            json{{"path", req.relLabel},
                 {"force", req.force},
                 {"reloaded", static_cast<u32>(total)},
                 {"textures", std::move(textures)},
                 {"models", std::move(models)},
                 {"reboundEntities", reboundEntities},
                 {"checkedTextures", r.checkedTextures},
                 {"checkedModels", r.checkedModels},
                 {"skipped", std::move(skipped)},
                 {"warnings", std::move(warnings)},
                 {"note", total == 0
                      ? "更新時刻が変わったアセットはありませんでした。"
                        "書き出し直したのに 0 件なら force:true を試すこと"
                      : "シーンは開き直していない。絵の確認は "
                        "dx12_screenshot_final (gizmos:false) で"}});
    }
}

} // namespace dx12e
