#include "editor/panels/SculptPanel.h"

#include "editor/EditorContext.h"
#include "editor/PropertyGrid.h"
#include "editor/RayGeometry.h"
#include "editor/UndoSystem.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Mesh.h"
#include "scene/Scene.h"
#include "terrain/SculptIO.h"
#include "terrain/SculptMesh.h"
// SkeletalAnimation の unique_ptr メンバを entt が完全型として要求するため
//（InspectorPanel と同じ理由・同じ並び）。
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace dx12e
{

namespace
{

// ==========================================================================
//  Undo: ストローク単位・触った頂点だけの差分
// ==========================================================================
// マウス押下〜離すで 1 エントリ。押下中に「触った代表頂点」の before を退避し、
// 離した時に同じ頂点の after を取る。全頂点スナップショットは 10 万頂点で 1.2MB/回になり
// 履歴 100 段で 120MB になるので採らない（地形のタイル差分と同じ考え方）。
class SculptStrokeCommand final : public IUndoCommand
{
public:
    SculptStrokeCommand(entt::registry* reg, entt::entity entity,
                        std::vector<u32> roots,
                        std::vector<XMFLOAT3> before,
                        std::vector<XMFLOAT3> after,
                        const char* label)
        : m_reg(reg), m_entity(entity), m_roots(std::move(roots)),
          m_before(std::move(before)), m_after(std::move(after)), m_label(label) {}

    void Undo() override { Apply(m_before); }
    void Redo() override { Apply(m_after); }
    const char* GetName() const override { return m_label; }

private:
    void Apply(const std::vector<XMFLOAT3>& src)
    {
        if (!m_reg || !m_reg->valid(m_entity)) return;
        auto* sc = m_reg->try_get<SculptMesh>(m_entity);
        if (!sc || !sc->_data || !sc->_data->IsValid()) return;
        if (src.size() != m_roots.size()) return;

        for (size_t i = 0; i < m_roots.size(); ++i)
            sc->_data->SetRootPosition(m_roots[i], src[i]);
        sc->_data->RecomputeNormals(m_roots);
        // メッシュ再構築・コライダー作り直し・.smsh 保存はここで予約するだけ
        //（実行は SculptPanel::Render / PhysicsSystem::Update）。
        sc->MarkDirty();
    }

    entt::registry*       m_reg;
    entt::entity          m_entity;
    std::vector<u32>      m_roots;
    std::vector<XMFLOAT3> m_before;
    std::vector<XMFLOAT3> m_after;
    const char*           m_label;
};

// ==========================================================================
//  パネル状態（パネルは 1 個で足りるので関数ローカル static で持つ）
// ==========================================================================
struct SculptToolState
{
    // 借り物ポインタ（Render が毎フレーム更新。ビューポートハンドラから使う）
    Scene*         scene = nullptr;
    EditorContext* ctx   = nullptr;
    bool           handlerRegistered = false;
    // 「選択がスカルプトに変わった瞬間」だけ窓を開くための前フレーム選択（TerrainPanel と同型）。
    entt::entity   prevSelected = entt::null;

    // ブラシ
    SculptBrushParams brush;
    bool brushEnabled = true;
    int  brushTypeIdx = 0;

    // 素体
    int  newPrimitiveIdx = 1;        // 既定 = 球（岩の素体として一番使う）
    int  newSubdivisions = 16;
    f32  newSize         = 2.0f;
    char newName[64]     = "Sculpt";

    // ストローク
    bool         strokeActive = false;
    entt::entity strokeEntity = entt::null;
    std::vector<u32>        strokeRoots;
    std::vector<XMFLOAT3>   strokeBefore;
    std::unordered_set<u32> strokeSeen;
    std::vector<u32>        gatherScratch;
    const char*             strokeLabel = "Sculpt";

    // Grab 用（ストローク中のみ有効。すべてローカル空間）
    XMFLOAT3 grabCenter{0.0f, 0.0f, 0.0f};   // ブラシ中心（掴んだ点。毎フレーム移動量ぶん動く）
    XMFLOAT3 grabPlaneP{0.0f, 0.0f, 0.0f};   // 引っぱり平面の点（押した瞬間のヒット点で固定）
    XMFLOAT3 grabPlaneN{0.0f, 0.0f, 1.0f};   // 同・法線（押した瞬間の視線方向で固定）
    XMFLOAT3 grabPrevPt{0.0f, 0.0f, 0.0f};   // 前フレームの平面上の点
};

SculptToolState& State()
{
    static SculptToolState s;
    return s;
}

const char* const kBrushLabels[] = {
    "盛る Draw", "引っぱる Pull", "押し込む Push", "ならす Smooth",
    "平らに Flatten", "つまむ Pinch", "ノイズ Noise", "掴む Grab"
};
const char* const kPrimitiveLabels[] = {
    "箱 Box", "球 Sphere", "板 Plane", "円柱 Cylinder"
};

// --- 対象のスカルプトエンティティ（選択中を優先、無ければシーンで最初に見つかったもの）---
entt::entity FindActiveSculpt(entt::registry& reg, const EditorContext& ctx)
{
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
        && reg.all_of<SculptMesh>(ctx.selectedEntity))
        return ctx.selectedEntity;

    auto view = reg.view<SculptMesh>();
    auto it   = view.begin();
    if (it != view.end()) return *it;
    return entt::null;
}

// ワールド座標 → スクリーン座標（ImGui 座標）。カメラ後方なら false。
bool WorldToScreen(const Camera& cam, const XMFLOAT3& world,
                   f32 vpX, f32 vpY, f32 vpW, f32 vpH, ImVec2& out)
{
    const XMVECTOR p    = XMVectorSet(world.x, world.y, world.z, 1.0f);
    const XMVECTOR clip = XMVector4Transform(p, cam.GetViewProjMatrix());
    const f32 w = XMVectorGetW(clip);
    if (w <= 1e-4f) return false;
    out.x = vpX + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * vpW;
    out.y = vpY + (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * vpH;
    return true;
}

// ローカル空間のレイ vs スカルプトメッシュ（三角形精密・両面）。
// 三角形を素直に総当たりする。スカルプトの素体はせいぜい数千〜数万三角形で、
// この関数はスカルプト窓を開いてビューポートにカーソルがある時しか走らないので許容する
//（BVH を積むのは「重い」と言われてからでよい）。
bool RaycastSculptLocal(const SculptMeshData& mesh, const XMFLOAT3& lo, const XMFLOAT3& ld,
                        XMFLOAT3& outLocal, XMFLOAT3& outNormal)
{
    const std::vector<XMFLOAT3>& pos = mesh.Positions();
    const std::vector<u32>&      idx = mesh.Indices();
    if (pos.empty() || idx.size() < 3) return false;

    XMFLOAT3 bmin{0.0f, 0.0f, 0.0f}, bmax{0.0f, 0.0f, 0.0f};
    mesh.Bounds(bmin, bmax);
    if (raygeo::RayAabb(lo, ld, bmin, bmax) < 0.0f) return false;

    const XMVECTOR o = XMLoadFloat3(&lo);
    const XMVECTOR d = XMLoadFloat3(&ld);

    f32 best    = -1.0f;
    u32 bestTri = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
    {
        const f32 tt = raygeo::RayTriangle(o, d,
                                           XMLoadFloat3(&pos[idx[t + 0]]),
                                           XMLoadFloat3(&pos[idx[t + 1]]),
                                           XMLoadFloat3(&pos[idx[t + 2]]));
        if (tt > 0.0f && (best < 0.0f || tt < best))
        {
            best    = tt;
            bestTri = static_cast<u32>(t / 3);
        }
    }
    if (best < 0.0f) return false;

    XMStoreFloat3(&outLocal, XMVectorAdd(o, XMVectorScale(d, best)));

    const size_t b  = static_cast<size_t>(bestTri) * 3;
    const XMVECTOR v0 = XMLoadFloat3(&pos[idx[b + 0]]);
    const XMVECTOR v1 = XMLoadFloat3(&pos[idx[b + 1]]);
    const XMVECTOR v2 = XMLoadFloat3(&pos[idx[b + 2]]);
    const XMVECTOR n  = XMVector3Cross(XMVectorSubtract(v1, v0), XMVectorSubtract(v2, v0));
    if (XMVectorGetX(XMVector3LengthSq(n)) > 1e-20f) XMStoreFloat3(&outNormal, XMVector3Normalize(n));
    else                                             outNormal = XMFLOAT3{0.0f, 1.0f, 0.0f};
    return true;
}

// レイ vs 平面（Grab の引っぱり量を決める）。平行なら false。
bool RayPlane(const XMFLOAT3& o, const XMFLOAT3& d,
              const XMFLOAT3& planeP, const XMFLOAT3& planeN, XMFLOAT3& out)
{
    const f32 denom = d.x * planeN.x + d.y * planeN.y + d.z * planeN.z;
    if (std::fabs(denom) < 1e-8f) return false;
    const f32 t = ((planeP.x - o.x) * planeN.x + (planeP.y - o.y) * planeN.y
                 + (planeP.z - o.z) * planeN.z) / denom;
    out = XMFLOAT3{ o.x + d.x * t, o.y + d.y * t, o.z + d.z * t };
    return true;
}

// 表面に沿った円と中心十字を描く（ローカル空間の円をワールドへ飛ばして投影する）。
// 描画先は必ず「メインビューポートの」背景 DrawList を明示指定すること。
// 引数無しの GetBackgroundDrawList() はマルチビューポート有効時に別 OS ウィンドウへ描かれる。
void DrawBrushCursor(const ViewportInput& in, const XMMATRIX& world,
                     const XMFLOAT3& localCenter, const XMFLOAT3& localNormal, f32 radius)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    if (!dl || !in.camera) return;

    // 法線に直交する正規直交基底
    XMVECTOR n = XMLoadFloat3(&localNormal);
    if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-12f) n = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    n = XMVector3Normalize(n);
    const XMVECTOR up = (std::fabs(XMVectorGetY(n)) > 0.99f) ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
                                                             : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR tx = XMVector3Cross(up, n);
    if (XMVectorGetX(XMVector3LengthSq(tx)) < 1e-12f) tx = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    tx = XMVector3Normalize(tx);
    const XMVECTOR tz = XMVector3Normalize(XMVector3Cross(n, tx));

    constexpr int kSegments = 48;
    ImVec2 pts[kSegments];
    const XMVECTOR c = XMLoadFloat3(&localCenter);
    for (int i = 0; i < kSegments; ++i)
    {
        const f32 a = static_cast<f32>(i) * (6.28318530718f / static_cast<f32>(kSegments));
        const XMVECTOR lp = XMVectorAdd(c,
            XMVectorAdd(XMVectorScale(tx, std::cos(a) * radius),
                        XMVectorScale(tz, std::sin(a) * radius)));
        XMFLOAT3 wp;
        XMStoreFloat3(&wp, XMVector3TransformCoord(lp, world));
        if (!WorldToScreen(*in.camera, wp, in.vpX, in.vpY, in.vpW, in.vpH, pts[i]))
            return;   // 一部でもカメラ後方なら描かない（画面を横切る変な線を出さない）
    }
    dl->AddPolyline(pts, kSegments, IM_COL32(126, 204, 255, 220), ImDrawFlags_Closed, 2.0f);

    XMFLOAT3 wc;
    XMStoreFloat3(&wc, XMVector3TransformCoord(c, world));
    ImVec2 sc;
    if (WorldToScreen(*in.camera, wc, in.vpX, in.vpY, in.vpW, in.vpH, sc))
    {
        dl->AddLine(ImVec2(sc.x - 6.0f, sc.y), ImVec2(sc.x + 6.0f, sc.y),
                    IM_COL32(126, 204, 255, 220), 1.5f);
        dl->AddLine(ImVec2(sc.x, sc.y - 6.0f), ImVec2(sc.x, sc.y + 6.0f),
                    IM_COL32(126, 204, 255, 220), 1.5f);
    }
}

// --- ストローク管理 ---
void BeginStroke(SculptToolState& s, entt::entity e, const char* label)
{
    s.strokeActive = true;
    s.strokeEntity = e;
    s.strokeLabel  = label;
    s.strokeRoots.clear();
    s.strokeBefore.clear();
    s.strokeSeen.clear();
}

void EndStroke(SculptToolState& s, entt::registry& reg, EditorContext& ctx)
{
    if (!s.strokeActive) return;
    s.strokeActive = false;

    SculptMesh* sc = reg.valid(s.strokeEntity) ? reg.try_get<SculptMesh>(s.strokeEntity) : nullptr;
    if (!sc || !sc->_data || !sc->_data->IsValid() || s.strokeRoots.empty())
    {
        s.strokeRoots.clear();
        s.strokeBefore.clear();
        s.strokeSeen.clear();
        return;
    }

    std::vector<XMFLOAT3> after;
    after.reserve(s.strokeRoots.size());
    bool changed = false;
    for (size_t i = 0; i < s.strokeRoots.size(); ++i)
    {
        const XMFLOAT3 p = sc->_data->GetRootPosition(s.strokeRoots[i]);
        after.push_back(p);
        if (!changed)
        {
            const XMFLOAT3& b = s.strokeBefore[i];
            if (p.x != b.x || p.y != b.y || p.z != b.z) changed = true;
        }
    }

    if (changed)
    {
        ctx.undoSystem.PushCommand(std::make_unique<SculptStrokeCommand>(
            &reg, s.strokeEntity, s.strokeRoots, s.strokeBefore, std::move(after), s.strokeLabel));
        // 形が確定した瞬間にだけコライダー作り直し + .smsh 保存を予約する。
        // ドラッグ中に毎フレーム MeshShape を作るとヒッチの元なので絶対にやらない。
        sc->MarkDirty();
    }

    s.strokeRoots.clear();
    s.strokeBefore.clear();
    s.strokeSeen.clear();
}

// .smsh を書き出す。パス未設定ならエンティティ名から決める。
// 頂点配列はシーン JSON に入らないので、彫ったら必ずこれを通す必要がある
//（通さないと Play→Stop のシーン再構築で保存済みの .smsh まで巻き戻る）。
bool SaveSculptAsset(entt::registry& reg, entt::entity e, SculptMesh& sc,
                     const std::string& assetsDir, bool logSuccess)
{
    if (!sc._data || !sc._data->IsValid()) return false;
    if (sc.meshPath.empty())
    {
        const std::string nm = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name
                                                      : std::string("Sculpt");
        sc.meshPath = sculpt::MakeSculptMeshRelPath(nm);
    }
    const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;
    if (!sculpt::SaveSculptMeshFile(base + sc.meshPath, *sc._data)) return false;
    if (logSuccess) Logger::Info("スカルプトメッシュを保存しました: {}", sc.meshPath);
    return true;
}

// 選択中エンティティの MeshRenderer（CPU キャッシュ）から編集可能なコピーを作る。
// 全サブメッシュを 1 つに畳み込み、meshNodeTransforms も焼き込む。
// 元の .glb 等には一切書き戻さない（読むだけ）。
bool MakeEditableFromEntity(entt::registry& reg, entt::entity src, SculptMeshData& out)
{
    auto* mr = reg.try_get<MeshRenderer>(src);
    if (!mr || mr->meshes.empty()) return false;

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> uvs;
    std::vector<u32>      indices;

    for (size_t mi = 0; mi < mr->meshes.size(); ++mi)
    {
        Mesh* m = mr->meshes[mi];
        if (!m) continue;
        const std::vector<XMFLOAT3>& sp = m->GetPositions();
        const std::vector<u32>&      si = m->GetIndices();
        if (sp.empty() || si.size() < 3) continue;

        XMMATRIX node = XMMatrixIdentity();
        if (mi < mr->meshNodeTransforms.size())
            node = XMLoadFloat4x4(&mr->meshNodeTransforms[mi]);

        const u32 base = static_cast<u32>(positions.size());
        const std::vector<Vertex>& sv = m->MutableVertices();   // 読むだけ（const 版が無い）
        for (size_t v = 0; v < sp.size(); ++v)
        {
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&sp[v]), node));
            positions.push_back(p);

            if (v < sv.size())
            {
                XMVECTOR n = XMVector3TransformNormal(XMLoadFloat3(&sv[v].normal), node);
                if (XMVectorGetX(XMVector3LengthSq(n)) > 1e-12f) n = XMVector3Normalize(n);
                else                                             n = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                XMFLOAT3 n3;
                XMStoreFloat3(&n3, n);
                normals.push_back(n3);
                uvs.push_back(sv[v].texCoord);
            }
            else
            {
                normals.push_back(XMFLOAT3{0.0f, 1.0f, 0.0f});
                uvs.push_back(XMFLOAT2{0.0f, 0.0f});
            }
        }
        for (const u32 i : si)
            indices.push_back(base + i);
    }

    if (positions.empty() || indices.size() < 3) return false;
    return out.BuildFrom(positions, normals, uvs, indices);
}

// ==========================================================================
//  ビューポートのブラシ操作（EditorContext::viewportToolHandlers に登録）
// ==========================================================================
bool HandleViewportBrush(const ViewportInput& in)
{
    SculptToolState& s = State();

    if (!s.scene || !s.ctx || !in.camera) return false;
    EditorContext& ctx = *s.ctx;
    if (!ctx.showSculptEditor || !s.brushEnabled) return false;
    // ギズモを掴んでいる間はそっちが優先（ブラシに横取りさせない）
    if (ImGuizmo::IsUsing()) return false;

    entt::registry& reg = s.scene->GetRegistry();
    // 地形を選んでいるなら地形ブラシに譲る（両窓を開いている時の食い合い防止。TerrainPanel 側と対）
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
        && reg.all_of<Terrain>(ctx.selectedEntity))
        return false;
    const entt::entity e = FindActiveSculpt(reg, ctx);
    if (e == entt::null) return false;

    SculptMesh* sc = reg.try_get<SculptMesh>(e);
    if (!sc || !sc->_data || !sc->_data->IsValid()) return false;
    SculptMeshData& mesh = *sc->_data;

    // --- ショートカット: [ ] で半径（地形ツールと同じ）---
    if (in.inViewport)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))
            s.brush.radius = (std::max)(0.001f, s.brush.radius * 0.85f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true))
            s.brush.radius = (std::min)(10000.0f, s.brush.radius * 1.18f);
    }

    // --- ワールド行列とその逆（レイをローカルへ落とす）---
    const XMMATRIX world = ComputeWorldMatrix(reg, e);
    XMVECTOR det = XMMatrixDeterminant(world);
    if (std::fabs(XMVectorGetX(det)) < 1e-12f) return false;   // 潰れた行列（スケール 0 など）
    const XMMATRIX invWorld = XMMatrixInverse(&det, world);

    XMFLOAT3 lo{0.0f, 0.0f, 0.0f}, ld{0.0f, 0.0f, 1.0f};
    XMStoreFloat3(&lo, XMVector3TransformCoord(
        XMVectorSet(in.rayOrigin.x, in.rayOrigin.y, in.rayOrigin.z, 1.0f), invWorld));
    XMStoreFloat3(&ld, XMVector3TransformNormal(
        XMVectorSet(in.rayDir.x, in.rayDir.y, in.rayDir.z, 0.0f), invWorld));

    XMFLOAT3 hitLocal{0.0f, 0.0f, 0.0f};
    XMFLOAT3 hitNormal{0.0f, 1.0f, 0.0f};
    const bool hit = in.inViewport && RaycastSculptLocal(mesh, lo, ld, hitLocal, hitNormal);

    const ImGuiIO& io = ImGui::GetIO();
    const bool invert    = io.KeyShift;   // Shift ドラッグで逆方向
    const bool smoothMod = io.KeyCtrl;    // Ctrl で一時的に「ならす」

    // --- ストローク開始 ---
    if (in.pressed && hit)
    {
        BeginStroke(s, e, "Sculpt");
        s.grabCenter = hitLocal;
        s.grabPlaneP = hitLocal;
        s.grabPrevPt = hitLocal;
        XMVECTOR pn = XMLoadFloat3(&ld);
        if (XMVectorGetX(XMVector3LengthSq(pn)) > 1e-12f) pn = XMVector3Normalize(pn);
        else                                              pn = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        XMStoreFloat3(&s.grabPlaneN, pn);
    }

    // --- 彫る ---
    if (s.strokeActive && (in.pressed || in.dragging))
    {
        SculptBrushParams p = s.brush;
        if (smoothMod) p.type = SculptBrushType::Smooth;

        XMFLOAT3 center = hitLocal;
        bool     apply  = hit;

        if (p.type == SculptBrushType::Grab)
        {
            // 掴んだ点を通り視線に垂直な平面の上で、カーソルの移動量をそのまま引っぱり量にする。
            // 表面から外れても掴み続けられる（地形ブラシと違い hit を要求しない）。
            XMFLOAT3 pt{0.0f, 0.0f, 0.0f};
            if (RayPlane(lo, ld, s.grabPlaneP, s.grabPlaneN, pt))
            {
                p.grabDelta = XMFLOAT3{ pt.x - s.grabPrevPt.x,
                                        pt.y - s.grabPrevPt.y,
                                        pt.z - s.grabPrevPt.z };
                s.grabPrevPt = pt;
                center       = s.grabCenter;
                s.grabCenter = XMFLOAT3{ s.grabCenter.x + p.grabDelta.x,
                                         s.grabCenter.y + p.grabDelta.y,
                                         s.grabCenter.z + p.grabDelta.z };
                apply = true;
            }
            else
            {
                apply = false;
            }
        }
        else
        {
            // Pull/Push は「当たった面の法線」を既定の向きにする＝ Draw と同じ感覚で押し引きできる
            p.direction = hitNormal;
        }

        if (apply)
        {
            // dt はフレーム時間。フレームレートで彫れる量が変わらないようにする
            //（長時間の停止で一気に彫れないよう上限も入れる）。
            const f32 dt = (std::min)(io.DeltaTime, 0.05f);

            // 触る頂点の「元の位置」を退避（Smooth は隣接も読むので少し広めに取る）。
            SculptBrushParams gatherP = p;
            gatherP.radius = p.radius * 1.25f;
            s.gatherScratch.clear();
            mesh.GatherAffected(gatherP, center, s.gatherScratch);
            for (const u32 root : s.gatherScratch)
            {
                if (!s.strokeSeen.insert(root).second) continue;
                s.strokeRoots.push_back(root);
                s.strokeBefore.push_back(mesh.GetRootPosition(root));
            }

            if (mesh.ApplyBrush(p, center, dt, invert, nullptr) > 0)
            {
                // 見た目だけ即反映する。コライダー作り直しと .smsh 保存はストローク終了時
                //（EndStroke の MarkDirty）にまとめる＝ドラッグ中に詰まらない。
                sc->MarkVisualDirty();
            }
        }
    }

    // --- ブラシカーソル ---
    if (hit || (s.strokeActive && s.brush.type == SculptBrushType::Grab))
    {
        const XMFLOAT3& cc = (s.strokeActive && s.brush.type == SculptBrushType::Grab)
                             ? s.grabCenter : hitLocal;
        DrawBrushCursor(in, world, cc, hitNormal, s.brush.radius);
    }

    // --- 消費判定（ここで true を返すと 3D ピッキング/UI 編集は行われない）---
    bool consume = false;
    if (in.pressed && hit) consume = true;
    if (s.strokeActive && (in.dragging || in.released)) consume = true;

    if (in.released) EndStroke(s, reg, ctx);
    return consume;
}

} // anonymous namespace

// ==========================================================================
//  パネル本体
// ==========================================================================
namespace SculptPanel
{

bool MakeEditable(entt::registry& reg, entt::entity src, SculptMeshData& out)
{
    return MakeEditableFromEntity(reg, src, out);
}

void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir,
            ID3D12GraphicsCommandList* cmd)
{
    SculptToolState& s = State();
    s.scene = &scene;
    s.ctx   = &ctx;

    if (!s.handlerRegistered)
    {
        s.handlerRegistered = true;
        ctx.viewportToolHandlers.push_back(&HandleViewportBrush);
    }

    entt::registry& reg = scene.GetRegistry();

    // SculptMesh を持つエンティティを**選び直した瞬間だけ**勝手に開く。
    // 毎フレーム無条件だと ✕ が効かない（TerrainPanel と同じ事故）。
    if (ctx.selectedEntity != s.prevSelected)
    {
        s.prevSelected = ctx.selectedEntity;
        if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
            && reg.all_of<SculptMesh>(ctx.selectedEntity))
            ctx.showSculptEditor = true;
    }

    // 頂点が変わったメッシュを作り直す（ブラシ / Undo / 素体作成の全経路がここに集まる）。
    // 窓が閉じていても回す＝Ctrl+Z の結果が必ず絵に出る。
    for (auto [e, sc] : reg.view<SculptMesh>().each())
    {
        if (sc._meshDirty) scene.RebuildSculptMesh(e, cmd);
        // ストロークが終わった（=マウスを離した）タイミングで .smsh を自動保存する。
        // 頂点配列はシーン JSON に入らないため、これをやらないと Play→Stop の
        // シーン再構築で彫った内容が「保存済みの .smsh」まで巻き戻ってしまう。
        if (sc._needsSave && !s.strokeActive)
        {
            sc._needsSave = false;
            SaveSculptAsset(reg, e, sc, assetsDir, /*logSuccess=*/false);
        }
    }

    if (!ctx.showSculptEditor) return;

    ImGui::SetNextWindowSize(ImVec2(430.0f, 700.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("スカルプト（異形）###SculptToolFloating", &ctx.showSculptEditor,
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    // 3D ビューの上に浮くので、ホバー中はカメラ操作を抑止する（他のフローティング窓と同じ流儀）
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    // ---------------- 素体を作成 ----------------
    // 注: 対象エンティティの解決（下）はこのブロックの後に行うこと。ここで SpawnSculpt すると
    //     SculptMesh のコンポーネントプールが再確保され、先に取った SculptMesh* が dangling になる。
    if (ImGui::CollapsingHeader("素体を作る", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("##SculptCreate"))
        {
            pg::InputText("名前", s.newName, sizeof(s.newName));
            pg::Combo("形", &s.newPrimitiveIdx, kPrimitiveLabels, IM_ARRAYSIZE(kPrimitiveLabels),
                      "岩は球、アーチ/柱は円柱、崖は箱から彫るのが早い");
            pg::SliderInt("分割数", &s.newSubdivisions, 1, 64, nullptr,
                          "細かいほど彫り込めるが重い（16〜24 が使いやすい）");
            pg::Float("大きさ(m)", &s.newSize, 0.1f, 0.05f, 500.0f, "%.2f");
            pg::End();
        }
        s.newPrimitiveIdx  = std::clamp(s.newPrimitiveIdx, 0,
                                        static_cast<int>(SculptPrimitive::Count) - 1);
        s.newSubdivisions  = std::clamp(s.newSubdivisions, 1, 64);

        if (ImGui::Button("＋ 素体を作成", ImVec2(-FLT_MIN, 0.0f)))
        {
            SculptMesh params;   // meshPath 空 = 素体から作る
            Entity created = scene.SpawnSculpt(
                s.newName[0] ? s.newName : "Sculpt",
                XMFLOAT3{0.0f, 0.0f, 0.0f}, params,
                static_cast<SculptPrimitive>(s.newPrimitiveIdx),
                static_cast<u32>(s.newSubdivisions), s.newSize, cmd);
            if (created.IsValid())
            {
                // ★TerrainPanel と同じ理由で Undo に積む（GUI 経由だけ抜けていた）。
                ctx.undoSystem.PushCommand(std::make_unique<SpawnEntityCommand>(
                    &scene, assetsDir, created.GetHandle()));
                ctx.Select(created.GetHandle());
                // ブラシ半径を素体の大きさに合わせる（毎回手で直さなくて済む）
                s.brush.radius   = s.newSize * 0.25f;
                s.brush.strength = s.newSize * 0.5f;
                Logger::Info("スカルプト素体を作成しました: {}", s.newName);
            }
        }

        // 「選択中のモデルを編集可能にする」
        const entt::entity sel = ctx.selectedEntity;
        const bool canConvert = (sel != entt::null) && reg.valid(sel)
                             && reg.all_of<MeshRenderer>(sel)
                             && !reg.all_of<SculptMesh>(sel);
        ImGui::BeginDisabled(!canConvert);
        if (ImGui::Button("選択中のモデルを編集可能にする", ImVec2(-FLT_MIN, 0.0f)))
        {
            auto data = std::make_shared<SculptMeshData>();
            if (MakeEditableFromEntity(reg, sel, *data))
            {
                const std::string srcName = reg.all_of<NameTag>(sel)
                                          ? reg.get<NameTag>(sel).name : std::string("Model");
                XMFLOAT3 pos{0.0f, 0.0f, 0.0f};
                Transform srcTf{};
                if (reg.all_of<Transform>(sel)) { srcTf = reg.get<Transform>(sel); pos = srcTf.position; }

                SculptMesh params;   // meshPath 空。素体は最小で作って直後に差し替える
                Entity created = scene.SpawnSculpt(srcName + "_Sculpt", pos, params,
                                                   SculptPrimitive::Plane, 1u, 1.0f, cmd);
                if (created.IsValid())
                {
                    auto& sc = created.GetComponent<SculptMesh>();
                    sc._data = data;
                    sc.MarkDirty();
                    // 元モデルと同じ姿勢に置く（頂点はローカル空間のまま焼いてあるので重なる）
                    if (created.HasComponent<Transform>())
                    {
                        Transform& tf = created.GetComponent<Transform>();
                        const entt::entity keepParent = tf.parent;
                        tf = srcTf;
                        tf.parent = keepParent;
                    }
                    scene.RebuildSculptMesh(created.GetHandle(), cmd);
                    ctx.undoSystem.PushCommand(std::make_unique<SpawnEntityCommand>(
                        &scene, assetsDir, created.GetHandle()));
                    ctx.Select(created.GetHandle());
                    if (reg.all_of<SkeletalAnimation>(sel))
                        Logger::Warn("スキン付きモデルを編集可能にしました。"
                                     "ボーンの追従は落ちます（静的な形として彫る前提やで）。");
                    Logger::Info("モデルを編集可能にしました: {} ({} verts)",
                                 srcName, sc._data->VertexCount());
                }
            }
            else
            {
                Logger::Warn("編集可能にできませんでした（CPU 側の頂点キャッシュが無いモデルです）。");
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("元の .glb は書き換えへん。コピーを彫る形式やで");
    }

    // ---- 対象の解決（エンティティ生成の後で取る＝ポインタが dangling しない）----
    const entt::entity active = FindActiveSculpt(reg, ctx);
    SculptMesh* target = (active != entt::null) ? reg.try_get<SculptMesh>(active) : nullptr;

    if (!target || !target->_data || !target->_data->IsValid())
    {
        ImGui::Separator();
        ImGui::TextWrapped("スカルプト対象がありません。上の「素体を作る」で作るか、"
                           "モデルを選んで「編集可能にする」を押してな。");
        ImGui::End();
        return;
    }

    SculptMeshData& mesh = *target->_data;

    ImGui::Separator();
    {
        const char* nm = reg.all_of<NameTag>(active) ? reg.get<NameTag>(active).name.c_str() : "?";
        XMFLOAT3 bmin{0.0f, 0.0f, 0.0f}, bmax{0.0f, 0.0f, 0.0f};
        mesh.Bounds(bmin, bmax);
        ImGui::Text("対象: %s", nm);
        ImGui::TextDisabled("%zu 頂点 / %zu 三角形 / サイズ %.2f x %.2f x %.2f",
                            mesh.VertexCount(), mesh.TriangleCount(),
                            bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z);
    }

    // ---------------- ブラシ ----------------
    if (ImGui::CollapsingHeader("スカルプトブラシ", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("##SculptBrush"))
        {
            pg::Checkbox("ブラシ有効", &s.brushEnabled,
                         "OFF にするとビューポートのクリックが通常の選択に戻る");
            pg::Combo("種類", &s.brushTypeIdx, kBrushLabels, IM_ARRAYSIZE(kBrushLabels));
            s.brushTypeIdx = std::clamp(s.brushTypeIdx, 0,
                                        static_cast<int>(SculptBrushType::Count) - 1);
            s.brush.type = static_cast<SculptBrushType>(s.brushTypeIdx);

            pg::Float("半径", &s.brush.radius, 0.01f, 0.001f, 1000.0f, "%.3f", nullptr,
                      "[ と ] でも増減できる");
            pg::Float("強さ", &s.brush.strength, 0.01f, 0.001f, 1000.0f, "%.3f");
            pg::SliderFloat("縁のぼかし", &s.brush.falloff, 0.0f, 1.0f, "%.2f", nullptr,
                            "0 = 硬い縁 / 1 = とろけるように滑らか");
            pg::Group("対称（ローカル軸）");
            pg::Checkbox("Xミラー", &s.brush.mirrorX, "x を反転した位置にも同じ筆を置く");
            pg::Checkbox("Yミラー", &s.brush.mirrorY);
            pg::Checkbox("Zミラー", &s.brush.mirrorZ);

            if (s.brush.type == SculptBrushType::Noise)
            {
                pg::Group("ノイズ");
                pg::Float("周波数", &s.brush.noiseFrequency, 0.01f, 0.001f, 100.0f, "%.3f");
                pg::SliderInt("オクターブ", &s.brush.noiseOctaves, 1, 8);
                pg::SliderFloat("尾根(ridged)", &s.brush.noiseRidged, 0.0f, 1.0f);
            }
            pg::End();
        }
        ImGui::TextDisabled("左ドラッグ=彫る / Shift=逆方向 / Ctrl=一時的にならす / [ ]=半径");
    }

    // ---------------- 見た目 / 当たり判定 / 保存 ----------------
    if (ImGui::CollapsingHeader("見た目・当たり判定・保存", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool visualChanged = false;
        bool colliderToggled = false;
        if (pg::Begin("##SculptLook"))
        {
            visualChanged |= pg::Color4("頂点色", &target->color.x);
            visualChanged |= pg::Float("UV倍率", &target->uvScale, 0.05f, 0.0f, 512.0f, "%.2f",
                                       nullptr, "元の UV に掛ける倍率（.dxmat のタイリング用）");
            colliderToggled = pg::Checkbox("当たり判定を作る", &target->collision,
                                           "静的な剛体なら三角形メッシュ形状（彫った通りに当たる）。"
                                           "動く剛体に付けると Jolt の制約で凸包になる");
            pg::Text("メッシュ", "%s",
                     target->meshPath.empty() ? "(未保存)" : target->meshPath.c_str());
            pg::End();
        }
        // 形は動いていないので MarkDirty（= .smsh 保存 + コライダー作り直し）ではなく
        // 見た目だけの再構築にする（スライダーをドラッグするたびに .smsh を書かない）。
        if (visualChanged)   target->MarkVisualDirty();
        if (colliderToggled) target->_colliderDirty = true;

        if (ImGui::Button("メッシュを保存 (.smsh)", ImVec2(-FLT_MIN, 0.0f)))
        {
            target->_needsSave = false;
            SaveSculptAsset(reg, active, *target, assetsDir, /*logSuccess=*/true);
        }
        ImGui::TextDisabled("彫り終わるたびに自動保存される（このボタンは手動の念押し）");
    }

    ImGui::End();
}

} // namespace SculptPanel

} // namespace dx12e
