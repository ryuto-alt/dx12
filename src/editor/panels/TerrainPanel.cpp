#include "editor/panels/TerrainPanel.h"

#include "editor/EditorContext.h"
#include "editor/PropertyGrid.h"
#include "editor/UndoSystem.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "scene/Scene.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainIO.h"
#include "terrain/TerrainSplatMap.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace dx12e
{

namespace
{

// ==========================================================================
//  Undo: ストローク単位のタイル差分
// ==========================================================================
// マウス押下〜離すで 1 エントリ。押下中に「触ったタイル」の before だけを退避し、
// 離した時に同じタイルの after を取る。全面スナップショットは解像度 512 で 1MB/回になり、
// 履歴 100 段で 200MB になるので採らない。
constexpr i32 kUndoTileSize = 64;

struct TerrainTileEdit
{
    i32 x0 = 0, z0 = 0, w = 0, h = 0;
    std::vector<f32> before;
    std::vector<f32> after;
};

class TerrainSculptCommand final : public IUndoCommand
{
public:
    TerrainSculptCommand(entt::registry* reg, entt::entity entity,
                         std::vector<TerrainTileEdit> tiles, const char* label)
        : m_reg(reg), m_entity(entity), m_tiles(std::move(tiles)), m_label(label) {}

    void Undo() override { Apply(true); }
    void Redo() override { Apply(false); }
    const char* GetName() const override { return m_label; }

private:
    void Apply(bool useBefore)
    {
        if (!m_reg || !m_reg->valid(m_entity)) return;
        auto* t = m_reg->try_get<Terrain>(m_entity);
        if (!t || !t->_hf || !t->_hf->IsValid()) return;

        for (const auto& tile : m_tiles)
        {
            const std::vector<f32>& src = useBefore ? tile.before : tile.after;
            const size_t need = static_cast<size_t>(tile.w) * static_cast<size_t>(tile.h);
            if (src.size() != need) continue;
            for (i32 z = 0; z < tile.h; ++z)
            {
                for (i32 x = 0; x < tile.w; ++x)
                {
                    const size_t si = static_cast<size_t>(z) * static_cast<size_t>(tile.w)
                                    + static_cast<size_t>(x);
                    t->_hf->SetAt(tile.x0 + x, tile.z0 + z, src[si]);
                }
            }
            // メッシュ再構築とコライダー作り直しはここで予約するだけ（実行は
            // TerrainPanel::Render / PhysicsSystem::Update）。
            t->MarkDirty(tile.x0, tile.z0, tile.x0 + tile.w - 1, tile.z0 + tile.h - 1);
        }
    }

    entt::registry*              m_reg;
    entt::entity                 m_entity;
    std::vector<TerrainTileEdit> m_tiles;
    const char*                  m_label;
};

// スプラット重み（RGBA8）のストローク単位 Undo。高さ版とまったく同じタイル方式。
// 1 タイル 64x64x4 = 16KB なので、履歴 100 段でも現実的なサイズに収まる。
struct SplatTileEdit
{
    i32 x0 = 0, z0 = 0, w = 0, h = 0;
    std::vector<u8> before;   // RGBA8
    std::vector<u8> after;
};

class TerrainPaintCommand final : public IUndoCommand
{
public:
    TerrainPaintCommand(entt::registry* reg, entt::entity entity, std::vector<SplatTileEdit> tiles)
        : m_reg(reg), m_entity(entity), m_tiles(std::move(tiles)) {}

    void Undo() override { Apply(true); }
    void Redo() override { Apply(false); }
    const char* GetName() const override { return "Terrain Paint"; }

private:
    void Apply(bool useBefore)
    {
        if (!m_reg || !m_reg->valid(m_entity)) return;
        auto* t = m_reg->try_get<Terrain>(m_entity);
        if (!t || !t->_splat || !t->_splat->IsValid()) return;

        const u32 size = t->_splat->Size();
        std::vector<u8>& px = t->_splat->Pixels();
        for (const auto& tile : m_tiles)
        {
            const std::vector<u8>& src = useBefore ? tile.before : tile.after;
            if (src.size() != static_cast<size_t>(tile.w) * tile.h * 4) continue;
            for (i32 z = 0; z < tile.h; ++z)
            {
                const size_t dstRow = (static_cast<size_t>(tile.z0 + z) * size
                                     + static_cast<size_t>(tile.x0)) * 4;
                const size_t srcRow = static_cast<size_t>(z) * tile.w * 4;
                std::memcpy(px.data() + dstRow, src.data() + srcRow,
                            static_cast<size_t>(tile.w) * 4);
            }
        }
        t->_splat->Touch();
        t->_splatNeedsSave = true;
    }

    entt::registry*            m_reg;
    entt::entity               m_entity;
    std::vector<SplatTileEdit> m_tiles;
};

// ==========================================================================
//  パネル状態（パネルは 1 個で足りるので関数ローカル static で持つ）
// ==========================================================================
struct TerrainToolState
{
    // 借り物ポインタ（Render が毎フレーム更新。ビューポートハンドラから使う）
    Scene*         scene = nullptr;
    EditorContext* ctx   = nullptr;
    bool           handlerRegistered = false;
    // 「選択が地形に変わった瞬間」だけ窓を開くための前フレーム選択。
    // 毎フレーム無条件に開くと ✕ が効かない（下の Render 参照）。
    entt::entity   prevSelected = entt::null;

    // ブラシ
    TerrainBrushParams brush;
    bool brushEnabled = true;
    int  brushTypeIdx = 0;

    // 新規作成
    int  newResolutionIdx = 2;      // kResolutionChoices の添字（既定 128）
    f32  newWorldSize     = 200.0f;
    char newName[64]      = "Terrain";

    // 生成（fBm プリセット）
    int  presetIdx = 2;             // 既定 = 険しい山脈
    int  genSeed   = 1337;
    TerrainGenParams gen;
    bool genInitialized = false;

    // 全面浸食
    int erodeIterations = 24;
    f32 erodeTalusDeg   = 34.0f;

    // テクスチャペイント（レイヤーセット割当時のみ）
    bool paintMode     = false;   // true の間、ブラシは高さではなくスプラット重みを塗る
    int  paintLayer    = 0;       // 0=草 1=土 2=岩 3=雪（レイヤーセットの並び）
    f32  paintStrength = 0.7f;

    // ストローク
    bool         strokeActive = false;
    entt::entity strokeEntity = entt::null;
    std::vector<TerrainTileEdit> strokeTiles;
    std::unordered_set<i64>      strokeTileKeys;
    const char*  strokeLabel = "Terrain Sculpt";

    // ペイントのストローク（高さとは独立に持つ）
    bool                       paintStrokeActive = false;
    entt::entity               paintStrokeEntity = entt::null;
    std::vector<SplatTileEdit> paintTiles;
    std::unordered_set<i64>    paintTileKeys;
};

TerrainToolState& State()
{
    static TerrainToolState s;
    return s;
}

const int kResolutionChoices[] = { 32, 64, 128, 256, 512 };
const char* const kResolutionLabels[] = { "32", "64", "128", "256", "512" };
const char* const kBrushLabels[] = {
    "盛る Raise", "削る Lower", "ならす Smooth", "平らに Flatten", "ノイズ Noise", "浸食 Erode"
};
const char* const kPresetLabels[] = {
    "なだらかな丘 Rolling Hills", "峡谷 Canyon", "険しい山脈 Mountains"
};

// --- 対象の地形エンティティ（選択中を優先、無ければシーンで最初に見つかったもの）---
entt::entity FindActiveTerrain(entt::registry& reg, const EditorContext& ctx)
{
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
        && reg.all_of<Terrain>(ctx.selectedEntity))
        return ctx.selectedEntity;

    auto view = reg.view<Terrain>();
    auto it   = view.begin();
    if (it != view.end()) return *it;
    return entt::null;
}

// 地形の原点（ワールド）。親子は考慮するが、回転/スケールはハイトフィールドの前提として無視する。
XMFLOAT3 TerrainOrigin(const entt::registry& reg, entt::entity e)
{
    XMFLOAT3 out{0.0f, 0.0f, 0.0f};
    if (!reg.all_of<Transform>(e)) return out;
    const XMMATRIX world = ComputeWorldMatrix(reg, e);
    XMStoreFloat3(&out, world.r[3]);
    return out;
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

// --- ストローク（Undo タイル）管理 ---
void CaptureTiles(TerrainToolState& s, const HeightField& hf, const HeightField::Rect& r)
{
    if (!r.Valid()) return;
    const i32 n = static_cast<i32>(hf.Resolution());
    const i32 tilesPerSide = (n + kUndoTileSize - 1) / kUndoTileSize;

    for (i32 tz = r.z0 / kUndoTileSize; tz <= r.z1 / kUndoTileSize; ++tz)
    {
        for (i32 tx = r.x0 / kUndoTileSize; tx <= r.x1 / kUndoTileSize; ++tx)
        {
            const i64 key = static_cast<i64>(tz) * static_cast<i64>(tilesPerSide)
                          + static_cast<i64>(tx);
            if (!s.strokeTileKeys.insert(key).second) continue;

            TerrainTileEdit tile;
            tile.x0 = tx * kUndoTileSize;
            tile.z0 = tz * kUndoTileSize;
            tile.w  = (std::min)(kUndoTileSize, n - tile.x0);
            tile.h  = (std::min)(kUndoTileSize, n - tile.z0);
            if (tile.w <= 0 || tile.h <= 0) continue;

            tile.before.resize(static_cast<size_t>(tile.w) * static_cast<size_t>(tile.h));
            for (i32 z = 0; z < tile.h; ++z)
            {
                for (i32 x = 0; x < tile.w; ++x)
                {
                    const size_t si = static_cast<size_t>(z) * static_cast<size_t>(tile.w)
                                    + static_cast<size_t>(x);
                    tile.before[si] = hf.At(tile.x0 + x, tile.z0 + z);
                }
            }
            s.strokeTiles.push_back(std::move(tile));
        }
    }
}

// --- ペイントのストローク（スプラットタイル）---
void CaptureSplatTiles(TerrainToolState& s, const TerrainSplatMap& sp, const HeightField::Rect& r)
{
    if (!r.Valid() || !sp.IsValid()) return;
    const i32 n = static_cast<i32>(sp.Size());
    const i32 tilesPerSide = (n + kUndoTileSize - 1) / kUndoTileSize;
    const std::vector<u8>& px = sp.Pixels();

    for (i32 tz = r.z0 / kUndoTileSize; tz <= r.z1 / kUndoTileSize; ++tz)
    {
        for (i32 tx = r.x0 / kUndoTileSize; tx <= r.x1 / kUndoTileSize; ++tx)
        {
            const i64 key = static_cast<i64>(tz) * static_cast<i64>(tilesPerSide)
                          + static_cast<i64>(tx);
            if (!s.paintTileKeys.insert(key).second) continue;

            SplatTileEdit tile;
            tile.x0 = tx * kUndoTileSize;
            tile.z0 = tz * kUndoTileSize;
            tile.w  = (std::min)(kUndoTileSize, n - tile.x0);
            tile.h  = (std::min)(kUndoTileSize, n - tile.z0);
            if (tile.w <= 0 || tile.h <= 0) continue;

            tile.before.resize(static_cast<size_t>(tile.w) * tile.h * 4);
            for (i32 z = 0; z < tile.h; ++z)
            {
                const size_t srcRow = (static_cast<size_t>(tile.z0 + z) * n
                                     + static_cast<size_t>(tile.x0)) * 4;
                std::memcpy(tile.before.data() + static_cast<size_t>(z) * tile.w * 4,
                            px.data() + srcRow, static_cast<size_t>(tile.w) * 4);
            }
            s.paintTiles.push_back(std::move(tile));
        }
    }
}

void BeginPaintStroke(TerrainToolState& s, entt::entity e)
{
    s.paintStrokeActive = true;
    s.paintStrokeEntity = e;
    s.paintTiles.clear();
    s.paintTileKeys.clear();
}

void EndPaintStroke(TerrainToolState& s, entt::registry& reg, EditorContext& ctx)
{
    if (!s.paintStrokeActive) return;
    s.paintStrokeActive = false;

    Terrain* t = reg.valid(s.paintStrokeEntity) ? reg.try_get<Terrain>(s.paintStrokeEntity) : nullptr;
    if (!t || !t->_splat || !t->_splat->IsValid() || s.paintTiles.empty())
    {
        s.paintTiles.clear();
        s.paintTileKeys.clear();
        return;
    }

    const i32 n = static_cast<i32>(t->_splat->Size());
    const std::vector<u8>& px = t->_splat->Pixels();
    bool changed = false;
    for (auto& tile : s.paintTiles)
    {
        tile.after.resize(tile.before.size());
        for (i32 z = 0; z < tile.h; ++z)
        {
            const size_t srcRow = (static_cast<size_t>(tile.z0 + z) * n
                                 + static_cast<size_t>(tile.x0)) * 4;
            std::memcpy(tile.after.data() + static_cast<size_t>(z) * tile.w * 4,
                        px.data() + srcRow, static_cast<size_t>(tile.w) * 4);
        }
        if (!changed && tile.after != tile.before) changed = true;
    }

    if (changed)
    {
        ctx.undoSystem.PushCommand(
            std::make_unique<TerrainPaintCommand>(&reg, s.paintStrokeEntity, std::move(s.paintTiles)));
    }
    s.paintTiles.clear();
    s.paintTileKeys.clear();
}

void BeginStroke(TerrainToolState& s, entt::entity e, const char* label)
{
    s.strokeActive = true;
    s.strokeEntity = e;
    s.strokeLabel  = label;
    s.strokeTiles.clear();
    s.strokeTileKeys.clear();
}

void EndStroke(TerrainToolState& s, entt::registry& reg, EditorContext& ctx)
{
    if (!s.strokeActive) return;
    s.strokeActive = false;

    Terrain* t = reg.valid(s.strokeEntity) ? reg.try_get<Terrain>(s.strokeEntity) : nullptr;
    if (!t || !t->_hf || !t->_hf->IsValid() || s.strokeTiles.empty())
    {
        s.strokeTiles.clear();
        s.strokeTileKeys.clear();
        return;
    }

    const HeightField& hf = *t->_hf;
    bool changed = false;
    for (auto& tile : s.strokeTiles)
    {
        tile.after.resize(tile.before.size());
        for (i32 z = 0; z < tile.h; ++z)
        {
            for (i32 x = 0; x < tile.w; ++x)
            {
                const size_t si = static_cast<size_t>(z) * static_cast<size_t>(tile.w)
                                + static_cast<size_t>(x);
                tile.after[si] = hf.At(tile.x0 + x, tile.z0 + z);
            }
        }
        if (!changed && tile.after != tile.before) changed = true;
    }

    if (changed)
    {
        ctx.undoSystem.PushCommand(std::make_unique<TerrainSculptCommand>(
            &reg, s.strokeEntity, std::move(s.strokeTiles), s.strokeLabel));
    }
    s.strokeTiles.clear();
    s.strokeTileKeys.clear();
}

// 全面操作（生成 / 全面浸食 / 全面ならし）を 1 コマンドとして積むヘルパ。
// 触るのは全タイルになるが、押しっぱなしのストロークと違い一度きりなので許容する。
template <typename Fn>
void RunWholeTerrainEdit(TerrainToolState& s, entt::registry& reg, EditorContext& ctx,
                         entt::entity e, const char* label, Fn&& op)
{
    Terrain* t = reg.valid(e) ? reg.try_get<Terrain>(e) : nullptr;
    if (!t || !t->_hf || !t->_hf->IsValid()) return;

    BeginStroke(s, e, label);
    CaptureTiles(s, *t->_hf, t->_hf->FullRect());
    op(*t->_hf);
    const HeightField::Rect full = t->_hf->FullRect();
    t->MarkDirty(full.x0, full.z0, full.x1, full.z1);
    EndStroke(s, reg, ctx);
}

// ハイトマップ(.hf)を書き出す。パス未設定ならエンティティ名から決める。
// 高さ配列はシーン JSON に入らないので、彫ったら必ずこれを通す必要がある
//（通さないと Play→Stop のシーン再構築で保存済みの .hf まで巻き戻る）。
bool SaveHeightmap(entt::registry& reg, entt::entity e, Terrain& t,
                   const std::string& assetsDir, bool logSuccess)
{
    if (!t._hf || !t._hf->IsValid()) return false;
    if (t.heightmapPath.empty())
    {
        const std::string nm = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name
                                                      : std::string("Terrain");
        t.heightmapPath = terrain::MakeHeightFieldRelPath(nm);
    }
    const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;
    if (!terrain::SaveHeightFieldFile(base + t.heightmapPath, *t._hf)) return false;
    if (logSuccess) Logger::Info("地形ハイトマップを保存しました: {}", t.heightmapPath);
    return true;
}

// スプラット重み(.splat)を書き出す。.hf とまったく同じ流儀（パス未設定なら名前から決める）。
// これを通さないと Play→Stop のシーン再構築でペイントが巻き戻る。
bool SaveSplat(entt::registry& reg, entt::entity e, Terrain& t,
               const std::string& assetsDir, bool logSuccess)
{
    if (!t._splat || !t._splat->IsValid()) return false;
    if (t.splatPath.empty())
    {
        const std::string nm = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name
                                                      : std::string("Terrain");
        t.splatPath = terrain::MakeSplatRelPath(nm);
    }
    const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;
    if (!terrain::SaveSplatMapFile(base + t.splatPath, *t._splat)) return false;
    if (logSuccess) Logger::Info("地形スプラットマップを保存しました: {}", t.splatPath);
    return true;
}

// 地形の上にブラシの円と中心十字を描く。
// 描画先は必ず「メインビューポートの」背景 DrawList を明示指定すること。
// 引数無しの GetBackgroundDrawList() はマルチビューポート有効時に別 OS ウィンドウへ描かれる。
void DrawBrushCursor(const ViewportInput& in, const HeightField& hf,
                     const XMFLOAT3& origin, f32 lcx, f32 lcz, f32 radius)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    if (!dl || !in.camera) return;

    constexpr int kSegments = 48;
    ImVec2 pts[kSegments];
    for (int i = 0; i < kSegments; ++i)
    {
        const f32 a  = static_cast<f32>(i) * (6.28318530718f / static_cast<f32>(kSegments));
        const f32 px = lcx + std::cos(a) * radius;
        const f32 pz = lcz + std::sin(a) * radius;
        const XMFLOAT3 w{ origin.x + px, origin.y + hf.SampleHeight(px, pz), origin.z + pz };
        if (!WorldToScreen(*in.camera, w, in.vpX, in.vpY, in.vpW, in.vpH, pts[i]))
            return;   // 一部でもカメラ後方なら描かない（画面を横切る変な線を出さない）
    }

    dl->AddPolyline(pts, kSegments, IM_COL32(255, 214, 92, 220), ImDrawFlags_Closed, 2.0f);

    const XMFLOAT3 center{ origin.x + lcx, origin.y + hf.SampleHeight(lcx, lcz), origin.z + lcz };
    ImVec2 c;
    if (WorldToScreen(*in.camera, center, in.vpX, in.vpY, in.vpW, in.vpH, c))
    {
        dl->AddLine(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x + 6.0f, c.y),
                    IM_COL32(255, 214, 92, 220), 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - 6.0f), ImVec2(c.x, c.y + 6.0f),
                    IM_COL32(255, 214, 92, 220), 1.5f);
    }
}

// ==========================================================================
//  ビューポートのブラシ操作（EditorContext::viewportToolHandlers に登録）
// ==========================================================================
bool HandleViewportBrush(const ViewportInput& in)
{
    TerrainToolState& s = State();

    if (!s.scene || !s.ctx || !in.camera) return false;
    EditorContext& ctx = *s.ctx;
    if (!ctx.showTerrainEditor || !s.brushEnabled) return false;
    // ギズモを掴んでいる間はそっちが優先（ブラシに横取りさせない）
    if (ImGuizmo::IsUsing()) return false;

    entt::registry& reg = s.scene->GetRegistry();
    // スカルプト対象を選んでいるなら、そっちを彫りたいという意思表示なので手を出さない
    // （両方の窓を開いていると、地形が先に登録されているぶん常にクリックを横取りしてしまう）
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
        && reg.all_of<SculptMesh>(ctx.selectedEntity))
        return false;
    const entt::entity e = FindActiveTerrain(reg, ctx);
    if (e == entt::null) return false;

    Terrain* t = reg.try_get<Terrain>(e);
    if (!t || !t->_hf || !t->_hf->IsValid()) return false;
    HeightField& hf = *t->_hf;

    // --- ショートカット: [ ] で半径 ---
    if (in.inViewport)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))
            s.brush.radius = (std::max)(0.5f, s.brush.radius * 0.85f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true))
            s.brush.radius = (std::min)(hf.WorldSize() * 0.5f, s.brush.radius * 1.18f);
    }

    // --- レイ → 地形（ローカル座標）---
    const XMFLOAT3 origin = TerrainOrigin(reg, e);
    const XMFLOAT3 localOrigin{ in.rayOrigin.x - origin.x,
                                in.rayOrigin.y - origin.y,
                                in.rayOrigin.z - origin.z };
    f32 hitT = 0.0f;
    const bool hit = in.inViewport && hf.Raycast(localOrigin, in.rayDir, 100000.0f, hitT);

    f32 lx = 0.0f, lz = 0.0f;
    if (hit)
    {
        lx = localOrigin.x + in.rayDir.x * hitT;
        lz = localOrigin.z + in.rayDir.z * hitT;
        DrawBrushCursor(in, hf, origin, lx, lz, s.brush.radius);
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool invert    = io.KeyShift;   // Shift ドラッグで逆方向（Raise↔Lower）
    const bool smoothMod = io.KeyCtrl;    // Ctrl で一時的に「ならす」

    // --- テクスチャペイント（高さブラシと排他）---
    // レイヤーセットが割り当たっていてペイントモードのときだけ、高さではなく
    // スプラット重みを塗る。ストローク単位 Undo・自動保存の作法は高さ側と同じ。
    if (s.paintMode && t->_splat && t->_splat->IsValid() && !t->layerSetPath.empty())
    {
        if (in.pressed && hit) BeginPaintStroke(s, e);

        if (s.paintStrokeActive && hit && (in.pressed || in.dragging))
        {
            // Shift で「そのレイヤーを消す」＝他レイヤーを持ち上げる代わりに、
            // 支配的な別レイヤーへ寄せる（レイヤー 0 へ戻す）。
            const u32 layer = invert ? 0u : static_cast<u32>(std::clamp(s.paintLayer, 0, 3));
            const f32 dt = (std::min)(io.DeltaTime, 0.05f);
            const f32 amount = std::clamp(s.paintStrength * dt * 6.0f, 0.0f, 1.0f);

            // 触ったタイルの before を退避してから塗る
            TerrainSplatMap& sp = *t->_splat;
            const f32 ws = hf.WorldSize();
            HeightField::Rect pre;
            pre.x0 = sp.LocalToTexelX(lx - s.brush.radius, ws);
            pre.z0 = sp.LocalToTexelZ(lz - s.brush.radius, ws);
            pre.x1 = sp.LocalToTexelX(lx + s.brush.radius, ws);
            pre.z1 = sp.LocalToTexelZ(lz + s.brush.radius, ws);
            CaptureSplatTiles(s, sp, pre);

            const HeightField::Rect painted =
                sp.PaintCircle(ws, lx, lz, s.brush.radius, layer, amount, s.brush.falloff);
            if (painted.Valid()) t->_splatNeedsSave = true;
        }

        bool consumePaint = false;
        if (in.pressed && hit) consumePaint = true;
        if (s.paintStrokeActive && (in.dragging || in.released)) consumePaint = true;
        if (in.released) EndPaintStroke(s, reg, ctx);
        return consumePaint;
    }

    // --- ストローク開始 ---
    if (in.pressed && hit)
    {
        BeginStroke(s, e, "Terrain Sculpt");
        // Flatten の目標高さは「ストローク開始時のブラシ中心の高さ」（業界標準の挙動）
        s.brush.flattenTarget = hf.SampleHeight(lx, lz);
    }

    // --- 塗る ---
    if (s.strokeActive && hit && (in.pressed || in.dragging))
    {
        TerrainBrushParams p = s.brush;
        if (smoothMod) p.type = TerrainBrushType::Smooth;
        p.maxHeight =  t->maxHeight;
        p.minHeight = -t->maxHeight;

        // dt はフレーム時間。フレームレートで彫れる量が変わらないようにする
        // （長時間の停止で一気に掘れないよう上限も入れる）。
        const f32 dt = (std::min)(io.DeltaTime, 0.05f);

        // 触ったタイルの before を退避（Smooth/Erode は隣接も読むので 1 セル広げる）。
        const f32 capR = p.radius + hf.CellSize();
        CaptureTiles(s, hf, hf.RectForCircle(lx, lz, capR));
        if (p.mirrorX)              CaptureTiles(s, hf, hf.RectForCircle(-lx,  lz, capR));
        if (p.mirrorZ)              CaptureTiles(s, hf, hf.RectForCircle( lx, -lz, capR));
        if (p.mirrorX && p.mirrorZ) CaptureTiles(s, hf, hf.RectForCircle(-lx, -lz, capR));

        const HeightField::Rect changed = ApplyTerrainBrush(hf, p, lx, lz, dt, invert);
        if (changed.Valid())
            t->MarkDirty(changed.x0, changed.z0, changed.x1, changed.z1);
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
namespace TerrainPanel
{

void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir,
            ID3D12GraphicsCommandList* cmd)
{
    TerrainToolState& s = State();
    s.scene = &scene;
    s.ctx   = &ctx;

    if (!s.handlerRegistered)
    {
        s.handlerRegistered = true;
        ctx.viewportToolHandlers.push_back(&HandleViewportBrush);
    }

    entt::registry& reg = scene.GetRegistry();

    // Terrain を持つエンティティを**選び直した瞬間だけ**勝手に開く。
    // ★以前は毎フレーム無条件に true を書いていた。この関数は窓が閉じていても
    //   毎フレーム呼ばれる（ApplicationRender.cpp）ので、✕ が false を書いても
    //   次のフレーム冒頭で true に戻され、**窓が閉じられなかった**。
    if (ctx.selectedEntity != s.prevSelected)
    {
        s.prevSelected = ctx.selectedEntity;
        if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
            && reg.all_of<Terrain>(ctx.selectedEntity))
            ctx.showTerrainEditor = true;
    }

    // 高さが変わったメッシュを作り直す（ブラシ / Undo / 生成の全経路がここに集まる）。
    // 窓が閉じていても回す＝Ctrl+Z の結果が必ず絵に出る。
    for (auto [e, t] : reg.view<Terrain>().each())
    {
        if (t._meshDirty) scene.RebuildTerrainMesh(e, cmd);
        // ストロークが終わった（=マウスを離した）タイミングで .hf を自動保存する。
        // 高さ配列はシーン JSON に入らないため、これをやらないと Play→Stop の
        // シーン再構築で彫った内容が「保存済みの .hf」まで巻き戻ってしまう。
        // ドラッグ中は書かない（毎フレーム数百KB書き出すのを避ける）。
        if (t._needsSave && !s.strokeActive)
        {
            t._needsSave = false;
            SaveHeightmap(reg, e, t, assetsDir, /*logSuccess=*/false);
        }
        // スプラット重み（.splat）も同じ理由で自動保存する（ドラッグ中は書かない）。
        if (t._splatNeedsSave && !s.paintStrokeActive)
        {
            t._splatNeedsSave = false;
            SaveSplat(reg, e, t, assetsDir, /*logSuccess=*/false);
        }
    }

    if (!ctx.showTerrainEditor) return;

    ImGui::SetNextWindowSize(ImVec2(430.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("地形ツール###TerrainToolFloating", &ctx.showTerrainEditor,
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

    // ---------------- 作成 ----------------
    // 注: 対象エンティティの解決（下）はこのブロックの後に行うこと。ここで SpawnTerrain すると
    //     Terrain のコンポーネントプールが再確保され、先に取った Terrain* が dangling になる。
    if (ImGui::CollapsingHeader("地形を作成", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const int resPick = kResolutionChoices[
            std::clamp(s.newResolutionIdx, 0, IM_ARRAYSIZE(kResolutionChoices) - 1)];

        if (pg::Begin("##TerrainCreate"))
        {
            pg::InputText("名前", s.newName, sizeof(s.newName));
            pg::Combo("解像度", &s.newResolutionIdx, kResolutionLabels,
                      IM_ARRAYSIZE(kResolutionLabels),
                      "1辺のサンプル数。細かいほど彫り込めるが重い（128 が使いやすい）");
            pg::Float("サイズ(m)", &s.newWorldSize, 1.0f, 4.0f, 4000.0f, "%.0f", nullptr,
                      "1辺のワールド長。セル幅 = サイズ / (解像度-1)");
            pg::Text("セル幅", "%.2f m",
                     s.newWorldSize / static_cast<f32>((std::max)(resPick - 1, 1)));
            pg::End();
        }

        if (ImGui::Button("＋ 地形を作成", ImVec2(-FLT_MIN, 0.0f)))
        {
            Terrain params;
            params.resolution = static_cast<u32>(resPick);
            params.worldSize  = s.newWorldSize;
            Entity created = scene.SpawnTerrain(s.newName[0] ? s.newName : "Terrain",
                                                XMFLOAT3{0.0f, 0.0f, 0.0f}, params, cmd);
            if (created.IsValid())
            {
                // ★Undo に積んでいなかった。誤って作った地形が Ctrl+Z で消えず、
                //   代わりに直前の別の編集が巻き戻っていた（MCP 経由の生成は
                //   ApplicationRender.cpp で SpawnEntityCommand を積んでいる＝GUI だけ抜けていた）。
                ctx.undoSystem.PushCommand(std::make_unique<SpawnEntityCommand>(
                    &scene, assetsDir, created.GetHandle()));
                ctx.Select(created.GetHandle());
                Logger::Info("地形を作成しました: {}", s.newName);
            }
        }
        ImGui::TextDisabled("床(Ground)と重なるときは、要らん床を消してや");
    }

    // ---- 対象の解決（エンティティ生成の後で取る＝ポインタが dangling しない）----
    const entt::entity active = FindActiveTerrain(reg, ctx);
    Terrain* tc = (active != entt::null) ? reg.try_get<Terrain>(active) : nullptr;

    if (!tc || !tc->_hf || !tc->_hf->IsValid())
    {
        ImGui::Separator();
        ImGui::TextWrapped("地形エンティティがありません。上の「地形を作成」で作ってな。");
        ImGui::End();
        return;
    }

    HeightField& hf = *tc->_hf;

    ImGui::Separator();
    {
        const char* nm = reg.all_of<NameTag>(active) ? reg.get<NameTag>(active).name.c_str() : "?";
        f32 hMin = 0.0f, hMax = 0.0f;
        hf.MinMaxHeight(hMin, hMax);
        ImGui::Text("対象: %s", nm);
        ImGui::TextDisabled("%u x %u / %.0fm / 高さ %.1f 〜 %.1f",
                            hf.Resolution(), hf.Resolution(), hf.WorldSize(), hMin, hMax);
    }

    // ---------------- ブラシ ----------------
    if (ImGui::CollapsingHeader("スカルプトブラシ", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("##TerrainBrush"))
        {
            pg::Checkbox("ブラシ有効", &s.brushEnabled,
                         "OFF にするとビューポートのクリックが通常の選択に戻る");
            pg::Combo("種類", &s.brushTypeIdx, kBrushLabels, IM_ARRAYSIZE(kBrushLabels));
            s.brushTypeIdx = std::clamp(s.brushTypeIdx, 0,
                                        static_cast<int>(TerrainBrushType::Count) - 1);
            s.brush.type = static_cast<TerrainBrushType>(s.brushTypeIdx);

            pg::Float("半径", &s.brush.radius, 0.25f, 0.5f, hf.WorldSize() * 0.5f, "%.1f m",
                      nullptr, "[ と ] でも増減できる");
            // ★浸食(Erode)だけは ApplyTerrainBrush がミラー処理より前で return しており
            //   （TerrainBrush.cpp の Erode 分岐）、strength / falloff / mirror / invert / dt を
            //   一切受け取らない。触っても何も起きないので触れないようにする。
            //   効かせるなら ApplyThermalErosion にこれらの受け口を足すところから。
            const bool erodeSelected = (s.brush.type == TerrainBrushType::Erode);
            ImGui::BeginDisabled(erodeSelected);
            pg::Float("強さ", &s.brush.strength, 0.1f, 0.1f, 200.0f, "%.1f");
            pg::SliderFloat("縁のぼかし", &s.brush.falloff, 0.0f, 1.0f, "%.2f", nullptr,
                            "0 = 硬い縁 / 1 = とろけるように滑らか");
            pg::Checkbox("Xミラー", &s.brush.mirrorX, "x を反転した位置にも同じ筆を置く");
            pg::Checkbox("Zミラー", &s.brush.mirrorZ);
            ImGui::EndDisabled();

            if (s.brush.type == TerrainBrushType::Noise)
            {
                pg::Group("ノイズ");
                pg::Float("周波数", &s.brush.noiseFrequency, 0.001f, 0.0001f, 1.0f, "%.4f");
                pg::SliderInt("オクターブ", &s.brush.noiseOctaves, 1, 8);
                pg::SliderFloat("尾根(ridged)", &s.brush.noiseRidged, 0.0f, 1.0f);
            }
            if (s.brush.type == TerrainBrushType::Erode)
            {
                pg::Group("浸食");
                pg::SliderFloat("安息角", &s.brush.talusDeg, 5.0f, 80.0f, "%.0f deg");
                pg::SliderInt("反復回数", &s.brush.erodeIterations, 1, 40);
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                                   "浸食は 強さ/ぼかし/ミラー/Shift を使いません");
            }
            pg::End();
        }
        if (s.brush.type == TerrainBrushType::Erode)
            ImGui::TextDisabled("左ドラッグ=浸食 / Ctrl=一時的にならす / [ ]=半径");
        else
            ImGui::TextDisabled("左ドラッグ=塗る / Shift=逆方向 / Ctrl=一時的にならす / [ ]=半径");
    }

    // ---------------- 山を一発生成 ----------------
    if (ImGui::CollapsingHeader("山を生成（fBm）", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!s.genInitialized)
        {
            s.gen = MakeTerrainPreset(static_cast<TerrainPreset>(s.presetIdx),
                                      hf.WorldSize(), static_cast<u32>(s.genSeed));
            s.genInitialized = true;
        }

        if (pg::Begin("##TerrainGen"))
        {
            if (pg::Combo("プリセット", &s.presetIdx, kPresetLabels, IM_ARRAYSIZE(kPresetLabels)))
            {
                s.presetIdx = std::clamp(s.presetIdx, 0,
                                         static_cast<int>(TerrainPreset::Count) - 1);
                s.gen = MakeTerrainPreset(static_cast<TerrainPreset>(s.presetIdx),
                                          hf.WorldSize(), static_cast<u32>(s.genSeed));
            }
            pg::Float("振幅(高さ)", &s.gen.amplitude, 0.5f, 0.0f, 1000.0f, "%.1f m");
            pg::Float("周波数", &s.gen.frequency, 0.0005f, 0.0001f, 1.0f, "%.4f", nullptr,
                      "小さいほど大きな起伏になる");
            pg::SliderInt("オクターブ", &s.gen.octaves, 1, 8);
            pg::SliderFloat("尾根(ridged)", &s.gen.ridged, 0.0f, 1.0f, "%.2f", nullptr,
                            "1 に近いほど鋭い尾根＝山脈らしくなる");
            pg::SliderFloat("外周を下げる", &s.gen.edgeFalloff, 0.0f, 1.0f);
            pg::SliderFloat("谷を掘る", &s.gen.valleyDepth, 0.0f, 3.0f);
            pg::Float("底上げ", &s.gen.baseHeight, 0.1f, -500.0f, 500.0f, "%.1f m");
            pg::Int("シード", &s.genSeed, 1.0f, 0, 1000000);
            pg::End();
        }
        s.gen.seed = static_cast<u32>((std::max)(s.genSeed, 0));

        if (ImGui::Button("この設定で生成（上書き）", ImVec2(-FLT_MIN, 0.0f)))
        {
            const TerrainGenParams gp = s.gen;
            RunWholeTerrainEdit(s, reg, ctx, active, "Terrain Generate",
                                [&gp](HeightField& target) { GenerateTerrain(target, gp); });
        }
        if (ImGui::Button("サイコロ（シードを変えて生成）", ImVec2(-FLT_MIN, 0.0f)))
        {
            const u32 next = static_cast<u32>(s.genSeed) * 1103515245u + 12345u;
            s.genSeed  = static_cast<int>(next & 0x7FFFFFFFu);
            s.gen.seed = static_cast<u32>(s.genSeed);
            const TerrainGenParams gp = s.gen;
            RunWholeTerrainEdit(s, reg, ctx, active, "Terrain Generate",
                                [&gp](HeightField& target) { GenerateTerrain(target, gp); });
        }
    }

    // ---------------- 全面操作 ----------------
    if (ImGui::CollapsingHeader("全体に適用"))
    {
        if (pg::Begin("##TerrainWhole"))
        {
            pg::SliderFloat("安息角", &s.erodeTalusDeg, 5.0f, 80.0f, "%.0f deg", nullptr,
                            "この傾きを超えた斜面の土砂が下へ落ちる（thermal erosion）");
            pg::SliderInt("反復回数", &s.erodeIterations, 1, 200);
            pg::End();
        }
        if (ImGui::Button("浸食をかける", ImVec2(-FLT_MIN, 0.0f)))
        {
            const f32 talus = s.erodeTalusDeg;
            const i32 iters = s.erodeIterations;
            RunWholeTerrainEdit(s, reg, ctx, active, "Terrain Erode",
                                [talus, iters](HeightField& target)
                                { ApplyThermalErosion(target, talus, iters, HeightField::Rect{}); });
        }
        if (ImGui::Button("平らに戻す", ImVec2(-FLT_MIN, 0.0f)))
        {
            RunWholeTerrainEdit(s, reg, ctx, active, "Terrain Flatten",
                                [](HeightField& target)
                                { std::fill(target.Heights().begin(),
                                            target.Heights().end(), 0.0f); });
        }
    }

    // ---------------- 見た目 / ハイトマップ保存 ----------------
    if (ImGui::CollapsingHeader("見た目とハイトマップ", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool visualChanged = false;
        if (pg::Begin("##TerrainLook"))
        {
            visualChanged |= pg::Color4("頂点色", &tc->color.x);
            visualChanged |= pg::Float("UVタイル数", &tc->uvScale, 0.25f, 0.0f, 512.0f, "%.1f",
                                       nullptr, "地形全体で UV が何回繰り返すか（.dxmat 用）");
            pg::Float("高さ上限", &tc->maxHeight, 1.0f, 1.0f, 5000.0f, "%.0f m", nullptr,
                      "ブラシで彫れる高さの絶対値の上限");
            pg::Text("ハイトマップ", "%s",
                     tc->heightmapPath.empty() ? "(未保存)" : tc->heightmapPath.c_str());
            pg::End();
        }
        // 高さは動いていないので MarkDirty（= .hf 保存 + コライダー作り直し）ではなく
        // 見た目だけの再構築にする（スライダーをドラッグするたびに .hf を書かない）。
        if (visualChanged) tc->MarkVisualDirty();

        if (ImGui::Button("ハイトマップを保存 (.hf)", ImVec2(-FLT_MIN, 0.0f)))
        {
            tc->_needsSave = false;
            SaveHeightmap(reg, active, *tc, assetsDir, /*logSuccess=*/true);
        }
        ImGui::TextDisabled("彫り終わるたびに自動保存される（このボタンは手動の念押し）");
    }

    // ---------------- テクスチャ（4 レイヤースプラット）----------------
    if (ImGui::CollapsingHeader("テクスチャ（レイヤー）", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const std::string prevLayerSet = tc->layerSetPath;
        bool flagsChanged = false;

        if (pg::Begin("##TerrainLayers"))
        {
            pg::InputTextStr("レイヤーセット", tc->layerSetPath, nullptr,
                             "assets 相対の .terrainlayers。空にすると従来の見た目（頂点色/.dxmat）に戻る");
            pg::Text("スプラット", "%s",
                     tc->splatPath.empty() ? "(未保存)" : tc->splatPath.c_str());

            if (!tc->layerSetPath.empty())
            {
                bool triplanar  = (tc->terrainMatFlags & 0x1u) != 0;
                bool pom        = (tc->terrainMatFlags & 0x2u) != 0;
                bool macro      = (tc->terrainMatFlags & 0x4u) != 0;
                bool distTiling = (tc->terrainMatFlags & 0x8u) != 0;
                pg::Group("繰り返し感の除去");
                flagsChanged |= pg::Checkbox("マクロ変化", &macro,
                    "低周波ノイズをアルベドに掛けて広い面の単調さを消す（タップ 0）");
                pg::SliderFloat("マクロ周期(m)", &tc->macroScale, 10.0f, 400.0f, "%.0f");
                pg::SliderFloat("マクロ強さ", &tc->macroStrength, 0.0f, 1.0f, "%.2f");
                flagsChanged |= pg::Checkbox("距離タイリング", &distTiling,
                    "遠景を粗いタイリングで重ねて格子模様を消す");
                pg::SliderFloat("距離開始(m)", &tc->distTilingStart, 5.0f, 200.0f, "%.0f");
                pg::SliderFloat("遠景の粗さ", &tc->distTilingFarScale, 2.0f, 16.0f, "%.1f");

                pg::Group("ブレンド / 投影");
                pg::SliderFloat("高さブレンド深さ", &tc->heightBlendDepth, 0.01f, 1.0f, "%.3f",
                                nullptr, "小さいほど境界がシャープ（1.0 でほぼ線形ブレンド）");
                flagsChanged |= pg::Checkbox("トライプラナー", &triplanar,
                    "急斜面のテクスチャ引き伸ばしを消す（急斜面のピクセルだけ 3 投影）");
                pg::SliderFloat("投影の鋭さ", &tc->triplanarSharpness, 1.0f, 16.0f, "%.1f");
                pg::SliderFloat("法線の強さ", &tc->normalStrength, 0.0f, 2.0f, "%.2f");

                pg::Group("パララックス（POM・既定 OFF）");
                flagsChanged |= pg::Checkbox("POM", &pom,
                    "寄った時だけ目地に立体感が出る。面積が広いのでコストは素直に効く");
                pg::SliderFloat("POM 高さ(m)", &tc->pomHeightScale, 0.0f, 0.3f, "%.3f");
                pg::SliderFloat("POM 開始(m)", &tc->pomFadeStart, 0.0f, 40.0f, "%.0f");
                pg::SliderFloat("POM 終了(m)", &tc->pomFadeEnd, 1.0f, 120.0f, "%.0f");

                if (flagsChanged)
                {
                    tc->terrainMatFlags = (triplanar  ? 0x1u : 0u) | (pom        ? 0x2u : 0u)
                                        | (macro      ? 0x4u : 0u) | (distTiling ? 0x8u : 0u);
                }
            }
            pg::End();
        }

        // レイヤーセットを付け外ししたら、スプラットを用意/破棄する（描画のゲートと同じ条件）。
        if (tc->layerSetPath != prevLayerSet)
        {
            if (tc->layerSetPath.empty())
            {
                tc->_splat.reset();   // 従来経路へ戻る
            }
            else if (!tc->_splat && tc->_hf)
            {
                tc->_splat = std::make_shared<TerrainSplatMap>();
                if (tc->splatPath.empty()
                    || !terrain::LoadSplatMapAsset(tc->splatPath, *tc->_splat))
                {
                    tc->_splat->Create(tc->splatResolution);
                    tc->_splat->AutoPaintFromHeightField(*tc->_hf, TerrainAutoPaintParams{});
                    tc->_splatNeedsSave = true;
                }
                tc->splatResolution = tc->_splat->Size();
            }
        }

        if (!tc->layerSetPath.empty())
        {
            // --- ペイントブラシ（ビューポートの左ドラッグを高さブラシから横取りする）---
            if (pg::Begin("##TerrainPaint"))
            {
                pg::Group("ペイントブラシ");
                pg::Checkbox("ペイントモード", &s.paintMode,
                    "ON の間、ビューポートの左ドラッグは高さではなくレイヤーの重みを塗る。"
                    "Shift ドラッグでレイヤー 0 へ戻す（消しゴム）");
                static const char* const kLayerLabels[] = { "0", "1", "2", "3" };
                pg::Combo("レイヤー", &s.paintLayer, kLayerLabels, 4,
                          "レイヤーセットの並び順。既定は 0=草 1=土 2=岩 3=雪");
                pg::SliderFloat("塗る強さ", &s.paintStrength, 0.05f, 1.0f, "%.2f");
                pg::SliderFloat("ブラシ半径", &s.brush.radius, 0.5f, 200.0f, "%.1f m", nullptr,
                                "高さブラシと共通（[ ] キーでも変えられる）");
                pg::End();
            }

            if (ImGui::Button("自動ペイント（傾斜と標高から）", ImVec2(-FLT_MIN, 0.0f)))
            {
                if (!tc->_splat) tc->_splat = std::make_shared<TerrainSplatMap>();
                if (!tc->_splat->IsValid()) tc->_splat->Create(tc->splatResolution);
                if (tc->_hf) tc->_splat->AutoPaintFromHeightField(*tc->_hf, TerrainAutoPaintParams{});
                tc->_splatNeedsSave = true;
            }
            if (ImGui::Button("スプラットを保存 (.splat)", ImVec2(-FLT_MIN, 0.0f)))
            {
                tc->_splatNeedsSave = false;
                SaveSplat(reg, active, *tc, assetsDir, /*logSuccess=*/true);
            }
            ImGui::TextDisabled("レイヤー 0=草 / 1=土 / 2=岩 / 3=雪 の並びを想定");
        }
        else
        {
            ImGui::TextDisabled("レイヤーセットを設定すると 4 層のテクスチャスプラットになる");
        }
    }

    ImGui::End();
}

} // namespace TerrainPanel

} // namespace dx12e
