#pragma once

// 任意メッシュの頂点スカルプト（Unreal Editor の VSculpt / ZBrush 系ブラシの最小実装）。
//
// ハイトフィールド地形（HeightField）は「XZ グリッドの高さ」しか持てないのでオーバーハングが
// 作れない。洞窟・アーチ・岩みたいな異形はこっちが担当する: 頂点位置を直接動かすので
// どんな形にでもなる代わりに、トポロジ（三角形の張り方）は一切変えない
// ＝インデックスが変わらないので GPU への部分更新（Mesh::UploadVertexCache）がそのまま使える。
//
// 座標系: 全部メッシュのローカル空間。エンティティの Transform は呼び出し側が面倒を見る。
//
// 【溶接（weld）について】GLB 等の DCC 出力は UV/法線シームで「同じ位置の頂点」が複数に割れて
// いる。そのまま彫ると継ぎ目がパックリ開くので、位置が一致する頂点を 1 グループにまとめて
// 代表（root）を決め、ブラシは root 単位で動かしてグループ全員へ同じ位置を書く。
// 法線の再計算も同じグループ単位。これで imported なモデルでも破綻せず彫れる。
//
// 【重み関数とノイズは地形と共有】フォールオフは TerrainBrushWeight、ノイズは TerrainFbm を
// そのまま使う（TerrainBrush.h の冒頭コメントに書いてある「持っていけばよい 2 つ」）。
//
// このファイルは純ロジック（GPU も entt も vfs も ImGui も使わない）。単体テストが
// SculptMesh.cpp を直接ビルドできるよう、依存は <DirectXMath> / core/Types.h /
// terrain/TerrainBrush.h（.cpp 側だけ）に保つこと。

#include <vector>
#include <cstddef>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

enum class SculptBrushType : int
{
    Draw    = 0,   // 法線方向に盛る（Inflate）。Shift で凹む
    Pull    = 1,   // 指定方向へ引っぱる
    Push    = 2,   // 指定方向の逆へ押し込む
    Smooth  = 3,   // ラプラシアン平滑化（隣接頂点の平均へ寄せる）
    Flatten = 4,   // 影響範囲の平均平面へ寄せる
    Pinch   = 5,   // ブラシ中心へ寄せる（稜線を立てる）
    Noise   = 6,   // fBm ノイズを法線方向へ足す（岩肌のざらつき）
    Grab    = 7,   // 掴んで引っぱる（フォールオフ付きの平行移動）
    Count   = 8,
};

enum class SculptPrimitive : int
{
    Box      = 0,
    Sphere   = 1,
    Plane    = 2,
    Cylinder = 3,
    Count    = 4,
};

struct SculptBrushParams
{
    SculptBrushType type = SculptBrushType::Draw;

    f32 radius   = 0.5f;    // ローカル単位の半径（球ブラシ）
    f32 strength = 1.0f;    // Draw/Pull/Push/Noise = 単位/秒、Smooth/Flatten/Pinch = 寄せる速さ
    f32 falloff  = 0.5f;    // 0 = 縁まで一定（円柱）、1 = とろけるように滑らか

    // 対称（ローカル空間のミラー）。X/Y/Z を同時に立てると最大 8 個の筆になる。
    bool mirrorX = false;
    bool mirrorY = false;
    bool mirrorZ = false;

    // Pull/Push が押し引きする向き（ローカル空間。長さは無視して正規化する）。
    DirectX::XMFLOAT3 direction{0.0f, 1.0f, 0.0f};
    // Grab がこのフレームに動かす量（ローカル空間）。dt は掛けない（すでに移動量そのもの）。
    DirectX::XMFLOAT3 grabDelta{0.0f, 0.0f, 0.0f};

    // Noise
    f32 noiseFrequency = 1.5f;
    i32 noiseOctaves   = 4;
    f32 noiseRidged    = 0.0f;
    u32 noiseSeed      = 1337u;
};

// TerrainFbm（2D）を XZ / XY / YZ の 3 平面で評価して平均した簡易 3D ノイズ（概ね -1..1）。
// 専用の 3D ノイズを足すのは「岩肌が縞に見える」と言われてからで十分。
f32 SculptFbm3(const DirectX::XMFLOAT3& p, f32 frequency, i32 octaves, f32 ridged, u32 seed);

class SculptMeshData
{
public:
    // 上限（壊れた .smsh で数百 MB 確保しにいかないための歯止め）
    static constexpr size_t kMaxVertices = 4000000;
    static constexpr size_t kMaxIndices  = 12000000;

    // ---- 生成 ----
    // 素体プリミティブ。subdivisions = 1 辺 / 1 周ぶんの分割数（大きいほど細かく彫れる）。
    // size = 一辺（Sphere/Cylinder は直径）のローカル長。法線は解析値をそのまま入れる
    // （Box はフラット、Sphere/Cylinder は滑らか。彫った所だけ後で再計算されて滑らかになる）。
    void BuildPrimitive(SculptPrimitive type, u32 subdivisions, f32 size);

    // 既存メッシュ（MeshRenderer の CPU キャッシュ）から編集可能なコピーを作る。
    // normals / uvs は頂点数が合っていなければ無視する（法線は再計算、UV は 0 埋め）。
    // 元アセット（.glb 等）には一切書き戻さない＝ここでコピーを持つのが「編集可能にする」の実体。
    bool BuildFrom(const std::vector<DirectX::XMFLOAT3>& positions,
                   const std::vector<DirectX::XMFLOAT3>& normals,
                   const std::vector<DirectX::XMFLOAT2>& uvs,
                   const std::vector<u32>& indices);

    bool IsValid() const
    {
        return !m_positions.empty()
            && m_indices.size() >= 3
            && (m_indices.size() % 3) == 0
            && m_normals.size() == m_positions.size()
            && m_uvs.size()     == m_positions.size()
            && m_weldRoot.size() == m_positions.size();
    }

    size_t VertexCount()   const { return m_positions.size(); }
    size_t TriangleCount() const { return m_indices.size() / 3; }
    size_t RootCount()     const { return m_roots.size(); }

    const std::vector<DirectX::XMFLOAT3>& Positions() const { return m_positions; }
    const std::vector<DirectX::XMFLOAT3>& Normals()   const { return m_normals; }
    const std::vector<DirectX::XMFLOAT2>& UVs()       const { return m_uvs; }
    const std::vector<u32>&               Indices()   const { return m_indices; }
    const std::vector<u32>&               Roots()     const { return m_roots; }

    // 生頂点 v が属する溶接グループの代表頂点番号。
    u32 RootOf(size_t v) const { return (v < m_weldRoot.size()) ? m_weldRoot[v] : 0u; }

    // 代表頂点 root に隣接する代表頂点の並び（インデックスから 1 回だけ作る）。
    // 個数を outCount へ返す。無ければ nullptr（outCount = 0）。
    const u32* NeighborsOf(u32 root, u32& outCount) const;
    // 代表頂点 root に接する三角形番号の並び（法線の部分再計算用）。
    const u32* TrianglesOf(u32 root, u32& outCount) const;

    // ---- 対称ミラー ----
    // ミラーで生まれる「同じ筆の鏡像」。中心・向き・掴み量を鏡像化して持つ。
    struct BrushInstance
    {
        DirectX::XMFLOAT3 center{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 direction{0.0f, 1.0f, 0.0f};
        DirectX::XMFLOAT3 grabDelta{0.0f, 0.0f, 0.0f};
    };
    static constexpr int kMaxBrushInstances = 8;   // X/Y/Z の 3 軸 → 2^3
    // out へミラー分を書き、個数（1..8）を返す。out[0] は必ずミラー無しの本体。
    static int BuildBrushInstances(const SculptBrushParams& p, const DirectX::XMFLOAT3& center,
                                   BrushInstance out[kMaxBrushInstances]);

    // ---- 編集 ----
    // ブラシを 1 回適用する（center はローカル座標、dt はストロークの連続適用量[秒]）。
    // invert=true で盛る↔削るを入れ替える（Shift ドラッグ用）。
    // outTouched へ動かした代表頂点番号を追記する（Undo の差分取得用。null 可）。
    // 戻り値 = 動かした代表頂点の延べ数（0 なら何も変わっていない）。
    size_t ApplyBrush(const SculptBrushParams& p, const DirectX::XMFLOAT3& center,
                      f32 dt, bool invert, std::vector<u32>* outTouched = nullptr);

    // ブラシが触れる代表頂点を列挙する（ミラー分も含む）。ApplyBrush の前に「元の位置」を
    // 退避するために使う（TerrainPanel の CaptureTiles と同じ役目）。
    void GatherAffected(const SculptBrushParams& p, const DirectX::XMFLOAT3& center,
                        std::vector<u32>& outRoots) const;

    // 代表頂点の位置を設定/取得（設定は溶接グループ全員へ同じ値を書く）。Undo/Redo が使う。
    void              SetRootPosition(u32 root, const DirectX::XMFLOAT3& pos);
    DirectX::XMFLOAT3 GetRootPosition(u32 root) const;

    // 指定した代表頂点のまわりだけ法線を作り直す（全頂点走査はしない）。
    void RecomputeNormals(const std::vector<u32>& roots);
    // 全頂点の法線を作り直す（生成直後 / 読み込み直後の 1 回だけ）。
    void RecomputeAllNormals();

    void Bounds(DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax) const;

    // ---- .smsh バイナリ（マジック "SMSH" + version + 頂点数 + インデックス数 + 各配列）----
    // シーン JSON には入れない（頂点数万で JSON が破綻する）。地形の .hf と同じ流儀。
    static constexpr u32    kMagic      = 0x48534D53u;   // 'SMSH'（リトルエンディアン）
    static constexpr u32    kVersion    = 1u;
    static constexpr size_t kHeaderSize = 16;            // magic + version + vertexCount + indexCount

    std::vector<u8> Encode() const;
    bool            Decode(const u8* data, size_t size);
    bool            Decode(const std::vector<u8>& bytes)
    {
        return Decode(bytes.empty() ? nullptr : bytes.data(), bytes.size());
    }

private:
    // 溶接 + 隣接（頂点→頂点 / 頂点→三角形）を作り直す。生成/読み込みの直後に 1 回だけ。
    void BuildTopology();
    // ミラー 1 個ぶんの適用（m_touched へ追記する）。
    void ApplyBrushAt(const SculptBrushParams& p, const BrushInstance& bi, f32 dt, bool invert);

    std::vector<DirectX::XMFLOAT3> m_positions;
    std::vector<DirectX::XMFLOAT3> m_normals;
    std::vector<DirectX::XMFLOAT2> m_uvs;
    std::vector<u32>               m_indices;

    // 溶接（同位置頂点のグループ）
    std::vector<u32> m_weldRoot;     // 生頂点 → 代表頂点
    std::vector<u32> m_roots;        // 代表頂点の一覧（ブラシはこれを走査する）
    std::vector<u32> m_groupStart;   // CSR: 代表頂点 → グループ内の生頂点（size = V+1）
    std::vector<u32> m_groupList;    // size = V

    // 隣接（CSR、代表頂点単位）
    std::vector<u32> m_adjStart;     // 頂点→頂点（size = V+1）
    std::vector<u32> m_adjList;
    std::vector<u32> m_triStart;     // 頂点→三角形（size = V+1）
    std::vector<u32> m_triList;

    // 作業用（毎フレーム確保し直さないよう持ち回す）
    std::vector<u32>               m_hitRoots;
    std::vector<f32>               m_hitWeights;
    std::vector<DirectX::XMFLOAT3> m_newPositions;
    std::vector<u32>               m_touched;
    std::vector<u32>               m_dirtyTris;
    std::vector<u32>               m_dirtyRoots;
    std::vector<u32>               m_triStamp;    // 法線再計算の重複除去（世代スタンプ）
    std::vector<u32>               m_vertStamp;
    u32                            m_stamp = 0;
};

} // namespace dx12e
