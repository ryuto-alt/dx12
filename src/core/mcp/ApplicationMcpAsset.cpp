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

    McpDefine("get_bounds", "entity:int,includeChildren:bool,name:string", DX12E_MCP_HANDLER
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

    McpDefine("move_asset", "from:string,overwrite:bool,to:string", DX12E_MCP_HANDLER
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
            resp["ok"] = true;
            resp["result"] = {{"from", fromRel}, {"to", toRel},
                              {"note", "シーン/プレハブ内の参照パスは自動更新されない(必要なら開き直して保存)"}};
        });

    McpDefine("delete_asset", "path:string,recursive:bool", DX12E_MCP_HANDLER
        {
            // assets 内のファイル削除。ディレクトリは recursive:true 必須(誤爆防止)。
            const std::string rel = ValidateMcpAssetRelPath(params.value("path", std::string()));
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "asset not found: " + rel);
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
                              {"note", "シーン/プレハブ内の参照パスは自動更新されない"}};
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



} // namespace dx12e
