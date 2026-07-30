#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <cstdint>
#include <functional>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e
{

class Mesh;
class HeightField;   // terrain/HeightField.h（Terrain コンポーネントが shared_ptr で持つ）
class TerrainSplatMap;  // terrain/TerrainSplatMap.h（同上。4 レイヤーのスプラット重み）
struct Material;
class Skeleton;
class AnimationClip;
class Animator;
class SkinningBuffer;
class NodeGraph;
class NodeAnimationClip;
class NodeAnimator;
struct AnimGraphRuntimeState;   // animation/AnimGraphRuntime.h（AnimatorController が持つ）

struct NameTag
{
    std::string name;
};

// エンティティの安定 ID。
//
// なぜ要るか: シーン JSON の親子は長らく「entities 配列のインデックス」で書かれていた。
// その index は entt のプール順（＝挿入順の逆を reverse したもの）に依存するので、
// 2 人が別々の場所にエンティティを足すと、git は行単位でどちらも通してしまい、
// **コンフリクトを一切出さずに以降の全 parent がズレて階層が壊れる**。
// 位置に依存しない参照が要る、というのがこの型の存在理由。
//
// 値は保存時に未設定のエンティティへ自動で振る（既存シーンは開いて保存すれば付く）。
// 0 は「未設定」の番兵。複製・プレハブ展開・貼り付けでは必ず振り直す
// （同じ guid のエンティティが 2 体できると参照先が曖昧になるため）。
//
// 64bit にしたのは JSON が短く済み、衝突が実用上あり得ないため
// （数百万エンティティでも誕生日衝突の確率は 1e-7 未満）。
struct EntityGuid
{
    uint64_t value = 0;
};

// タグ集合。固定命名(filter="Player" 等)を一般化する汎用ラベル。
// 群(RTS)管理・対象指定・クエリに使う。scene.QueryByTag("enemy") で列挙。データのみ。
struct Tag
{
    std::vector<std::string> tags;
};

// 汎用データ値。数値/真偽/文字列/Vec3 を1つ持つ（Lua定義のゲーム状態の単位）。
struct DataValue
{
    enum class Type : uint8_t { Number = 0, Bool = 1, String = 2, Vec3 = 3 };
    Type              type = Type::Number;
    double            num  = 0.0;
    bool              b    = false;
    std::string       str;
    DirectX::XMFLOAT3 vec{0.0f, 0.0f, 0.0f};
};

// 自由形式のキー値ストア。盤面セル/HP/陣営/手札 などゲーム状態をエンジン無改造で持つ器。
// 固定フィールドではなく動的キーなので Inspector 自動UI(entt::meta)の対象外。Lua から読み書きする。
struct DataComponent
{
    std::unordered_map<std::string, DataValue> values;
};

struct Transform
{
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f};  // Euler degrees（エディタ表示用）
    DirectX::XMFLOAT3 scale    = {1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 quaternion = {0.0f, 0.0f, 0.0f, 1.0f}; // (x,y,z,w) 物理同期用
    bool useQuaternion = false; // true なら quaternion から行列を生成

    // 親子階層
    entt::entity parent = entt::null;

    DirectX::XMMATRIX GetWorldMatrix() const;  // ローカル行列（親は考慮しない）
};

// 親階層を遡って合成したワールド行列を返す（描画/ギズモ/ピッキング用）
DirectX::XMMATRIX ComputeWorldMatrix(const entt::registry& reg, entt::entity e);

// エンティティ名で結ばれた参照を追従させる（リネーム時に呼ぶ）。
//
// シーンには「名前でエンティティを指す」参照が 3 種類ある:
//   - LuaScript の公開プロパティ（type="entity"）
//   - Trigger::filter（反応する対象。空は "Player" の暗黙指定なので触らない）
//   - TriggerAction::target（アクションの対象）
// これらはリネームすると**黙って切れる**（解決は実行時の名前一致なので、
// エラーも出ずに「なぜか動かない」だけになる）。
//
// ★暫定対処。本来は guid で結ぶべきで、EntityGuid の導入はその第一歩。
//   ここは「guid 化が全参照へ行き渡るまで、少なくともリネームでは壊れない」を担保する。
//   .uianim のトラック target（アセット側の名前パス）はシーン外なので対象外。
void RewriteEntityNameRefs(entt::registry& reg, const std::string& oldName,
                           const std::string& newName);

// 16 桁 hex 文字列 ⇔ guid。JSON では**数値ではなく文字列**で持つ
// （u64 を JSON 数値で書くと JS/TS 側が double へ丸めて下位ビットを失う）。
// 壊れた文字列は 0（＝未設定）を返す。呼び出し側で例外を捕まえなくてよい。
uint64_t    ParseEntityGuidHex(const std::string& s);
std::string FormatEntityGuidHex(uint64_t v);

// guid からエンティティを引く（0 / 見つからない → entt::null）。線形走査。
// 名前引き（NameTag の文字列比較）と同じ計算量だが、リネームでも同名でも揺れない。
entt::entity FindEntityByGuid(const entt::registry& reg, uint64_t guid);

// 参照の解決口。**guid が正、名前はフォールバック**。
// guid が 0（旧データ）か、guid が指す先が既に消えている場合だけ名前で引く。
entt::entity ResolveEntityRef(const entt::registry& reg, uint64_t guid, const std::string& name);

// 名前参照を guid へ昇格させる（メモリ上だけ。ファイルは触らない）。対象は
// Trigger の filter/target と、LuaScript の type="entity" プロパティ。
// シーン読み込みの最後と保存の直前に呼ぶ。これで旧シーンも「開いて保存」で自動移行し、
// そのセッションで作った参照も 1 往復待たずに guid が付く。
// 名前が解決できないもの（＝参照切れ）は guid 0 のまま残すので、診断は従来どおり拾える。
void PromoteEntityRefsToGuid(entt::registry& reg);

// サブツリー展開（複製 / 貼り付け / プレハブの新規配置）で「別のエンティティ」を作るとき、
// **サブツリー内部を指していた guid** を 0 へ落とす。落とさないと参照が
// 「切れる」のではなく「複製元の方を正しく指し続ける」＝より悪い失敗になる。
// 0 にしておけば名前フォールバックへ落ち、連番リネーム後の名前付け替えが効く。
// srcGuids は展開元 JSON に入っていた guid の集合。
void ClearInternalEntityRefGuids(entt::registry& reg,
                                 const std::vector<entt::entity>& created,
                                 const std::unordered_set<uint64_t>& srcGuids);

struct MeshRenderer
{
    std::string modelPath; // アセット相対パス（シーン保存/読み込み用）
    std::vector<Mesh*>     meshes;
    std::vector<Material*> materials;
    std::vector<DirectX::XMFLOAT4X4> meshNodeTransforms;

    // エディタ用 PBR オーバーライド（Material ポインタに依存しない直接値）
    float overrideMetallic  = -1.0f;  // < 0 = Material の値を使う
    float overrideRoughness = -1.0f;

    // UV タイリング（頂点バッファへ焼き込む方式。値を変えると Mesh::ApplyUVScale で VB を作り直す）
    float uvScaleU = 1.0f;
    float uvScaleV = 1.0f;

    // --- UVスクロール(uv/秒)。滝・溶岩・コンベア・流れる雲など。頂点は触らず、シェーダーの
    // b2 ルート定数 uvScaleOffset に毎フレーム値を積むだけなので VB 再生成は起きない。
    // uvScaleU/V(タイリング)とは併用できる(タイル済みの UV の上をスクロールする)。
    // 既定 Forward / ForwardSkinned で効く。カスタムシェーダーは自前で b2 を読む必要がある。
    // Lua `scene:setMeshUvScroll(entity, u, v)`
    float uvScrollU = 0.0f;
    float uvScrollV = 0.0f;

    // --- 連番アニメ(スプライトシート)。animFrames>0 で有効。アルベドを animCols x N グリッドと
    // みなし frame = floor(t*animFps) のセルだけを UV に写す(炎/水しぶき/爆発の板ポリ、
    // アニメする看板やモニタ)。有効時は uvScaleU/V と uvScrollU/V より優先される。
    // UV 計算は Sprite2D/UIImage と同じ renderer/SpriteAnim.h の純関数。Lua `scene:setMeshAnim`
    int   animFrames = 0;      // 総フレーム数(0=アニメなし)
    float animFps    = 8.0f;   // 再生速度(フレーム/秒)
    int   animCols   = 0;      // シートの列数(0=animFramesと同じ=横1行ストリップ)
    int   animRow    = 0;      // シート内の開始行(複数アニメを行で並べたシート用)
    int   animRows   = 0;      // シートの総行数(0=自動: animRow + ceil(animFrames/animCols))
    int   animMode   = 0;      // 0=ループ 1=単発(最終フレームで停止) 2=往復(ピンポン)
    // 再生位置(秒。非シリアライズ)。Application::Update が毎フレーム dt を足す
    float _animT = 0.0f;

    // カスタムシェーダー割当（プロジェクト assets/shaders/ 相対パス、空 = 既定の Forward）。
    // 静的メッシュのみ対応（スキンド/インスタンシングは既定へフォールバック）。ShaderManager 経由で
    // 実行時コンパイル・ホットリロードされる。Registry(エンジン組み込みシェーダー)と一致するパスは
    // 上書き扱いになり全体に効くため、個別割当の選択肢からは除外する(InspectorPanel側)。
    std::string shaderPath;

    // カスタムシェーダーのアルファブレンド有効化(shaderPath 指定時のみ意味を持つ)。
    // false(既定) = 不透明固定(BlendEnable=FALSE、DepthWrite=ON)。PSシェーダーが float4 の
    // alpha を書いても Forward 既定 PSO と同様に無視される(これが「カスタムシェーダーでアルファが
    // 効かない」不具合の原因やった)。true にすると SrcAlpha/InvSrcAlpha の通常アルファブレンドで
    // DepthWrite=OFF(半透明物の定石、ForwardGrid と同じ考え方)の専用 PSO を使う。
    bool shaderAlphaBlend = false;

    // カスタムシェーダーへ渡す汎用の進捗/強度値(0..1等、意味はシェーダー依存)。Sprite2D::effectValue と
    // 同じ役割のメッシュ版。Lua `scene:setMeshEffect(entity, value)` で実行時に書き換え可能
    // (ルート定数なので毎フレーム呼んでも安価、VB再生成なし)。既定シェーダー(Forward)では未使用。
    float effectValue = 0.0f;

    // カスタムシェーダーへ渡す汎用パラメーター4つ(意味はシェーダー依存。色/速度/しきい値等)。
    // HLSL側は cbuffer PerObjectConstants の effectValue の後ろに float4 shaderParams; を足して読む
    // (docs/AUTHORING.md 参照)。Inspector の Shader セクションで調整、Lua `scene:setMeshParams` でも
    // 書き換え可能(effectValue と同じルート定数経路 = 毎フレーム安価)。既定シェーダーでは未使用。
    DirectX::XMFLOAT4 shaderParams{0.0f, 0.0f, 0.0f, 0.0f};

    // マテリアルのテクスチャ差し替え（アセットブラウザからテクスチャをドラッグ&ドロップして割当。
    // Unity/Unreal 風）。サブメッシュ単位（meshes[]と同じインデックス）。空文字列 = Material 既定の
    // テクスチャを使う。**Mesh::GetMaterial() は同一モデルパスの全インスタンスで共有される
    // (ResourceManager のモデルキャッシュ由来）ため、ここを直接書き換えると他のインスタンスにも
    // 波及してしまう。代わりにこの override をインスタンス単位で保持し、描画時に専用の SRV ブロックを
    // 合成して差し替える(overrideMetallic/overrideRoughness と同じ「Material に触らず上書き」方針)。
    // インデックスは自動で足りない分を空文字列で埋める(必要時に resize)。
    std::vector<std::string> overrideAlbedoTexture;
    std::vector<std::string> overrideNormalTexture;
    std::vector<std::string> overrideMetalRoughnessTexture;

    // マテリアルアセット割当(assets/materials/*.dxmat、サブメッシュ単位、Unrealのマテリアルインスタンス
    // 相当)。空文字列 = 未割当。優先度は materialAsset > overrideXxxTexture(上記3ベクタ) > モデル焼き込み
    // Material。実データの解決/SRV構築は Application::m_materialAssetManager(MaterialAssetManager)が行う。
    std::vector<std::string> materialAsset;

    // 上記4ベクタの範囲外アクセスを避けるためのヘルパ(未設定インデックスは空文字列扱い)
    static const std::string& SafeGetOverride(const std::vector<std::string>& v, u32 mi)
    {
        static const std::string kEmpty;
        return (mi < v.size()) ? v[mi] : kEmpty;
    }
    static void SetOverride(std::vector<std::string>& v, u32 mi, const std::string& path)
    {
        if (v.size() <= mi) v.resize(mi + 1);
        v[mi] = path;
    }
    bool HasAnyTextureOverride(u32 mi) const
    {
        return !SafeGetOverride(overrideAlbedoTexture, mi).empty()
            || !SafeGetOverride(overrideNormalTexture, mi).empty()
            || !SafeGetOverride(overrideMetalRoughnessTexture, mi).empty();
    }
    bool HasMaterialAsset(u32 mi) const
    {
        return !SafeGetOverride(materialAsset, mi).empty();
    }

    // インスタンシング: 共有メッシュを使う発光弾(Pfx)等は色を頂点バッファに焼かず
    // ここに持つ（setColor が書き込む）。instanced=true の間 setColor は VB を再生成しない。
    DirectX::XMFLOAT4 instanceColor = {1.0f, 1.0f, 1.0f, 1.0f};
    bool instanced = false;

    // 一律色ティント(scene:setColor / シーンJSONの "color")。頂点色は共有メッシュに焼かれるため
    // メッシュ側からは「意図した割当」を復元できない(モデル本来の頂点色と区別が付かない)。
    // ここに保持してシリアライズすることで、エディタ保存やPlayスナップショット往復でも
    // モデルへの色指定が消えないようにする(2026-07-17: タイトルロゴの色がSave経由で消えた対策)。
    DirectX::XMFLOAT4 colorTint = {1.0f, 1.0f, 1.0f, 1.0f};
    bool hasColorTint = false;
};

struct SkeletalAnimation
{
    std::unique_ptr<Skeleton>       skeleton;
    std::unique_ptr<Animator>       animator;
    std::unique_ptr<SkinningBuffer> skinningBuffer;
    std::vector<std::unique_ptr<AnimationClip>> clips;

    SkeletalAnimation() = default;
    ~SkeletalAnimation();
    SkeletalAnimation(SkeletalAnimation&&) noexcept;
    SkeletalAnimation& operator=(SkeletalAnimation&&) noexcept;
};

// -------------------------------------------------------------------------
// アニメーションステートマシン。SkeletalAnimation と同居して Animator を上書き駆動する。
//
// グラフの構造（ステート・遷移・条件・ブレンドツリー・レイヤー・マスク・イベント）は
// すべて .animfsm（JSON アセット）側にあり、ここはパスと実行時パラメータだけを持つ。
// 反射シリアライズが std::vector を扱えないことへの対応であり、同時に
// 「ゲームの中身は全部データ」という設計思想への整合でもある（.uianim / .spranim と同型）。
//
// このコンポーネントが無ければ Animator は従来どおり単一クリップ + CrossFadeTo で動く
// （＝既存シーンは一切壊れない）。
// -------------------------------------------------------------------------
struct AnimatorController
{
    std::string graphPath;            // assets 相対（例 "animfsm/humanoid_locomotion.animfsm"）。空 = 無効
    bool        playOnStart = true;
    f32         speed       = 1.0f;   // グラフ全体の再生速度倍率
    bool        applyRootMotion = false;  // 計画05 Step 9（未実装。既定 false のまま）
    std::string eventChannel;         // アニメイベント名の前置き（空なら素の名前で EventBus へ）

    // ランタイム専有（非シリアライズ・meta 未登録・複製時に必ず無効化）
    std::unique_ptr<AnimGraphRuntimeState> _state;
    bool _loaded = false;   // graphPath のロードを試したか（毎フレームの I/O を防ぐ）
    bool _failed = false;   // ロードに失敗した（再試行しない。パスを変えると再試行する）
    std::string _loadedPath;

    // ⚠️ 特殊メンバは**全部**アウトオブライン定義（Components.cpp）にすること。
    //    AnimGraphRuntimeState はここでは前方宣言なので、インラインで = default にすると
    //    「例外時に unique_ptr メンバを破棄するコード」を各 TU が実体化しようとして
    //    `can't delete an incomplete type` になる（ComponentMeta.cpp が実際に踏んだ）。
    AnimatorController();
    ~AnimatorController();
    AnimatorController(AnimatorController&&) noexcept;
    AnimatorController& operator=(AnimatorController&&) noexcept;

    // 複製（Ctrl+D / プレハブ展開 / シリアライズの値コピー）では実行状態を引き継がない。
    // _state を共有すると 2 体が同じ FSM を回して壊れる。コピー先は次の Update で読み直す。
    AnimatorController(const AnimatorController& other);
    AnimatorController& operator=(const AnimatorController& other);
};

// -------------------------------------------------------------------------
// フット IK（接地補正）。SkeletalAnimation が必須。
// 地面へレイキャストして足首の高さと向きを合わせ、届かない側に合わせて腰を下げる。
// ボーン名が空なら一般的な命名から自動推定する（mixamorig: / .L .R / Bip01 等）。
//
// ⚠️ Play 中しか動かない（PhysicsSystem が body を持つのが Play 中だけのため）。
//    エディタのシーンビューでは接地しない。仕様。
// -------------------------------------------------------------------------
struct FootIK
{
    bool enabled = true;
    f32  weight  = 1.0f;        // 0..1 全体の効き

    // ---- ボーン指定（空なら自動推定。推定結果は _*Index に入る）----
    std::string leftHipBone, leftKneeBone, leftFootBone, leftToeBone;
    std::string rightHipBone, rightKneeBone, rightFootBone, rightToeBone;
    std::string pelvisBone;     // 腰下げの対象（空ならルートボーン）

    // ---- パラメータ ----
    f32 rayUpOffset     = 0.5f;   // 足首から何 m 上からレイを打つか
    f32 rayLength       = 1.0f;   // レイの全長
    f32 footHeight      = 0.10f;  // レストポーズでの足首の地面からの高さ
    f32 maxPelvisDrop   = 0.5f;   // 腰を下げられる上限(m)。これを超える段差は諦める
    f32 maxFootPitchDeg = 45.0f;  // 面法線に合わせる足の傾きの上限(度)
    f32 smoothTime      = 0.10f;  // 足首高さ/腰オフセットの指数平滑の時定数(秒)
    f32 fadeOutTime     = 0.15f;  // 非接地時に IK を切るフェード時間(秒)
    bool alignToNormal  = true;   // 面法線に足を合わせるか
    DirectX::XMFLOAT3 kneeForward{0.0f, 0.0f, 1.0f};  // 膝が向く方向（モデル空間）

    // ---- ランタイム専有（非シリアライズ・meta 未登録）----
    i32  _lHip = -1, _lKnee = -1, _lFoot = -1, _lToe = -1;
    i32  _rHip = -1, _rKnee = -1, _rFoot = -1, _rToe = -1;
    i32  _pelvis = -1;
    bool _resolved = false;
    bool _resolveFailed = false;
    f32  _lLift = 0.0f, _rLift = 0.0f;      // 平滑済みの足首の上げ下げ量
    f32  _pelvisDrop = 0.0f;                // 平滑済みの腰下げ量（0 以下）
    f32  _lWeight = 0.0f, _rWeight = 0.0f;  // フェード済みの片足ごとの効き
    bool _lContact = false, _rContact = false;
    bool _smoothInit = false;
    DirectX::XMFLOAT3 _lNormal{0.0f, 1.0f, 0.0f};
    DirectX::XMFLOAT3 _rNormal{0.0f, 1.0f, 0.0f};
};

struct NodeAnimationComp
{
    std::unique_ptr<NodeGraph>    nodeGraph;
    std::unique_ptr<NodeAnimator> nodeAnimator;
    std::vector<std::unique_ptr<NodeAnimationClip>> clips;

    NodeAnimationComp() = default;
    ~NodeAnimationComp();
    NodeAnimationComp(NodeAnimationComp&&) noexcept;
    NodeAnimationComp& operator=(NodeAnimationComp&&) noexcept;
};

struct GridPlane
{
    bool enabled = true;
};

// エディタ用グリッド床の一辺の長さ(m)。原点中心に ±kEditorGridSize/2(=10km) をカバーする。
// 以前は 500m で、遠くへ飛ぶとグリッドが「終わって」しまっていた。板を最初から
// 実用上無限のサイズにし、シェーダ(ForwardGrid.hlsl)側が「カメラの周りだけ」線を出す
// (フェード半径はカメラ高度に比例)＝どこまで飛んでも足元にグリッドがある。
// 板は2三角形・深度書き込み無し・線以外 alpha=0 なので、大きくしても描画コストは変わらない。
// spawn/save/load で必ず同じ値を使うこと。
inline constexpr f32 kEditorGridSize = 20000.0f;

// --- ハイトフィールド地形（山を作る道具。Unity Terrain / UE Landscape と同じ方式）---
//
// 描画は「普通の Mesh を持つ普通のエンティティ」として行う（MeshRenderer に CPU 生成の
// メッシュを載せる）。これで描画リスト構築・カリング・影・ピッキング・マテリアル割当が
// 全部そのまま乗る。コリジョンは Jolt の HeightFieldShape（PhysicsSystem::RegisterBody が
// この部品を見て作る）＝同じ高さ配列を見るので、彫れば当たり判定も自動で追従する。
//
// 高さ配列そのものはシーン JSON に入れない（解像度 256 で 6 万要素＝JSON が破綻する）。
// assets/terrain/<name>.hf にバイナリで置き、ここはパスだけを持つ（読み込みは vfs 経由）。
inline constexpr const char* kTerrainMeshMarker = "__terrain__";

struct Terrain
{
    u32  resolution = 128;       // 1 辺のサンプル数。HeightField::NormalizeResolution で 4 の倍数へ丸まる
    f32  worldSize  = 200.0f;    // 1 辺のワールド長(m)。セル幅 = worldSize/(resolution-1)
    f32  maxHeight  = 200.0f;    // ブラシの高さクランプ上限（±この値）
    std::string heightmapPath;   // assets 相対（例 "terrain/Terrain.hf"）。空 = 未保存（平坦で復元）
    f32  uvScale    = 24.0f;     // 地形全体で UV が何回繰り返すか（タイリングテクスチャ用）
    DirectX::XMFLOAT4 color{0.42f, 0.50f, 0.32f, 1.0f};  // 頂点色（マテリアル未割当時の見た目）

    // ---- テクスチャスプラット（4 レイヤー）----
    // ★layerSetPath が空なら従来経路（Forward.hlsl + .dxmat / 頂点色）へ落ちる＝完全後方互換。
    //   空でないときだけ地形専用 PSO（Terrain.hlsl）が選ばれる。
    std::string layerSetPath;      // assets 相対 ".terrainlayers"。空 = スプラットを使わない
    std::string splatPath;         // assets 相対 ".splat"。空 = 未保存
    f32  heightBlendDepth   = 0.2f;   // Mishkinis の depth（小さいほど境界がシャープ）
    f32  triplanarSharpness = 4.0f;   // トライプラナー重みの指数
    // bit0=triplanar bit1=POM bit2=macro bit3=distTiling。既定: トライプラナー/マクロ/距離タイリング ON
    u32  terrainMatFlags    = 0x0Du;
    f32  macroScale         = 90.0f;  // マクロバリエーションの周期(m)
    f32  macroStrength      = 0.45f;
    f32  distTilingStart    = 40.0f;  // ここから遠距離タイリングへブレンドし始める(m)
    f32  distTilingFarScale = 7.0f;   // 遠距離側は 1/この値のタイリング（粗くする）
    f32  normalStrength     = 1.0f;
    f32  pomHeightScale     = 0.05f;  // POM の高さ(m)。★配線は完成している
                                      //   （ApplicationRender.cpp:445 → Terrain.hlsl:67,409）。
                                      //   「効かない」のではなく POM 自体が既定 OFF なだけ
    f32  pomFadeStart       = 8.0f;
    f32  pomFadeEnd         = 25.0f;
    u32  pomMaxSteps        = 24;
    u32  splatResolution    = 512;    // .splat の 1 辺（2 のべき乗へ丸まる）

    // ---- ランタイム専有（非シリアライズ・meta 未登録）----
    // 高さ配列の実体。描画メッシュ生成・コリジョン・ブラシが同じインスタンスを見る。
    std::shared_ptr<HeightField> _hf;
    // スプラット重みの実体（4 レイヤー RGBA8）。描画とペイントが同じインスタンスを見る。
    std::shared_ptr<TerrainSplatMap> _splat;
    bool _splatNeedsSave = false;  // .splat を書き出す必要がある（TerrainPanel が消化）
    bool _meshDirty     = false;   // 高さが変わった → メッシュを作り直す
    bool _colliderDirty = false;   // 高さが変わった → 物理形状を作り直す（Play 中の追従用）
    // 高さが変わった → .hf を書き出す必要がある。高さ配列はシーン JSON に入らないので、
    // これを消化しないと Play→Stop のシーン再構築で「保存済みの .hf」まで彫った内容が
    // 巻き戻ってしまう（TerrainPanel がストローク終了時に消化して自動保存する）。
    bool _needsSave     = false;
    // 直近の変更で汚れたグリッド矩形（メッシュ部分更新用。x1 < x0 なら無効＝全面扱いしない）
    i32 _dirtyX0 = 0, _dirtyZ0 = 0, _dirtyX1 = -1, _dirtyZ1 = -1;

    void MarkDirty(i32 x0, i32 z0, i32 x1, i32 z1)
    {
        if (x1 < x0 || z1 < z0) return;
        if (_dirtyX1 < _dirtyX0 || _dirtyZ1 < _dirtyZ0)
        {
            _dirtyX0 = x0; _dirtyZ0 = z0; _dirtyX1 = x1; _dirtyZ1 = z1;
        }
        else
        {
            if (x0 < _dirtyX0) _dirtyX0 = x0;
            if (z0 < _dirtyZ0) _dirtyZ0 = z0;
            if (x1 > _dirtyX1) _dirtyX1 = x1;
            if (z1 > _dirtyZ1) _dirtyZ1 = z1;
        }
        _meshDirty     = true;
        _colliderDirty = true;
        _needsSave     = true;
    }

    void ClearDirtyRect() { _dirtyX0 = 0; _dirtyZ0 = 0; _dirtyX1 = -1; _dirtyZ1 = -1; }

    // 見た目だけ（頂点色 / UV タイリング）の変更。高さは動いていないので、
    // .hf の保存も物理形状の作り直しも要らない（矩形無効 = 全面再構築）。
    void MarkVisualDirty() { ClearDirtyRect(); _meshDirty = true; }
};

// --- 任意メッシュの頂点スカルプト（洞窟・アーチ・岩みたいな異形を作る道具）---
//
// ハイトフィールド地形はオーバーハングが作れないので、そこはこっちが担当する。
// 地形とまったく同じ構図: 描画は「普通の Mesh を持つ普通のエンティティ」（MeshRenderer に
// CPU 生成メッシュを載せる）＝描画リスト構築・カリング・影・ピッキング・マテリアル割当が
// そのまま乗る。コリジョンは Jolt の MeshShape（PhysicsSystem::RegisterBody がこの部品を
// 見て作る）で、ストローク終了時に作り直す＝彫った形に当たり判定が追従する。
//
// 頂点配列そのものはシーン JSON に入れない（数万頂点で JSON が破綻する）。
// assets/sculpt/<name>.smsh にバイナリで置き、ここはパスだけを持つ（読み込みは vfs 経由）。
class SculptMeshData;   // terrain/SculptMesh.h（頂点配列 + 溶接/隣接 + ブラシの実体）
inline constexpr const char* kSculptMeshMarker = "__sculpt__";

struct SculptMesh
{
    std::string meshPath;        // assets 相対（例 "sculpt/Rock.smsh"）。空 = 未保存（素体で復元）
    f32  uvScale   = 1.0f;       // UV の倍率（.dxmat のタイリング用）
    bool collision = true;       // 彫った形の MeshShape コライダーを作るか
    DirectX::XMFLOAT4 color{0.72f, 0.70f, 0.66f, 1.0f};  // 頂点色（マテリアル未割当時の見た目）

    // ---- ランタイム専有（非シリアライズ・meta 未登録）----
    // 頂点配列の実体。描画メッシュ生成・コリジョン・ブラシが同じインスタンスを見る。
    std::shared_ptr<SculptMeshData> _data;
    bool _meshDirty     = false;   // 頂点が変わった → GPU の頂点バッファを送り直す
    bool _colliderDirty = false;   // 頂点が変わった → 物理形状を作り直す（ストローク終了時のみ）
    // 頂点配列はシーン JSON に入らないため、これを消化しないと Play→Stop のシーン再構築で
    // 彫った内容が「保存済みの .smsh」まで巻き戻ってしまう（SculptPanel が自動保存する）。
    bool _needsSave     = false;

    // 彫った（＝形が変わった）。コライダーの作り直しと .smsh 保存も要る。
    void MarkDirty()       { _meshDirty = true; _colliderDirty = true; _needsSave = true; }
    // 見た目だけ（頂点色 / UV）の変更。形は変わっていないので物理も保存も要らない。
    void MarkVisualDirty() { _meshDirty = true; }
};

struct PointLight
{
    DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    f32 range     = 10.0f;
    bool castShadows = false;  // true でこのライトが影を落とす（同時上限あり、カメラ近い順で優先）
};

struct DirectionalLight
{
    DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
    DirectX::XMFLOAT3 color     = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    f32 ambient   = 0.25f;   // このライトが供給するシーン全体の環境光（影部分の明るさ）

    // Transform 回転の変化分を direction に反映するための前フレーム回転（非シリアライズ）
    DirectX::XMFLOAT3 _prevRot{0.0f, 0.0f, 0.0f};
    bool              _prevRotInit = false;
};

// スポットライト。位置は Transform、円錐の軸方向は direction。
// inner..outer の角度間で明るさが落ちる（Unity の Spot に相当）。
struct SpotLight
{
    DirectX::XMFLOAT3 color        = {1.0f, 1.0f, 1.0f};
    f32               intensity    = 3.0f;
    f32               range        = 15.0f;
    DirectX::XMFLOAT3 direction    = {0.0f, -1.0f, 0.0f};
    f32               innerConeDeg = 18.0f;  // この角度内は最大輝度
    f32               outerConeDeg = 28.0f;  // この角度でゼロまで減衰
    bool              castShadows  = false;  // true でこのライトが影を落とす（同時上限あり、カメラ近い順で優先）

    // Transform 回転の変化分を direction に反映するための前フレーム回転（非シリアライズ）
    DirectX::XMFLOAT3 _prevRot{0.0f, 0.0f, 0.0f};
    bool              _prevRotInit = false;
};

// 投影方式。Perspective=透視(3D)、Orthographic=正射(2D/見下ろし/ボード/RTS俯瞰)。
enum class CameraProjection : uint8_t
{
    Perspective  = 0,
    Orthographic = 1,
};

struct CameraComponent
{
    f32  fovDegrees = 60.0f;   // Perspective: 垂直FOV(度)
    f32  nearClip   = 0.1f;
    f32  farClip    = 1000.0f;
    bool isActive   = false;
    // 能力カタログ(Phase4): 正射投影。2D/2.5D・ボード・RTS俯瞰をエンジン無改造で成立させる。
    CameraProjection projection = CameraProjection::Perspective;
    f32  orthoSize  = 10.0f;    // Orthographic: ビュー縦の半分の世界単位(Unity orthographicSize 相当)
};

// 2Dスプライト(能力カタログ Phase4)。texturePath のテクスチャを、worldSpace=true なら
// アクティブカメラ連動のワールド座標で(2D/2.5D)、false なら画面HUDとして描く。
// layer は描画順(小さいほど奥)。uvMin/uvMax でアトラス切り出し、color は乗算色。
// データのみ。描画は SpriteRenderer が Sprite2D を走査して供給する。
struct Sprite2D
{
    std::string       texturePath;          // assets 相対パス
    int               layer    = 0;         // 描画順(昇順)
    DirectX::XMFLOAT2 size{1.0f, 1.0f};     // 世界/画面サイズ
    DirectX::XMFLOAT2 uvMin{0.0f, 0.0f};    // アトラス切り出し
    DirectX::XMFLOAT2 uvMax{1.0f, 1.0f};
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f}; // 乗算色(a=不透明度)
    bool              worldSpace = true;    // true=カメラ連動ワールド / false=HUD
    bool              billboard  = false;   // worldSpace時: true=常にカメラ正対(3D内マーカー等)

    // カスタムシェーダー割当(project assets/shaders/-相対、空=既定Sprite.hlsl)。worldSpaceのみ対応。
    // 実行時コンパイル/ホットリロードはMeshRendererと同じShaderManager経由(Application::EnsureCustomSpritePso)。
    // 頂点/ルートシグネチャ契約はメッシュ用シェーダーと異なる(docs/AUTHORING.md参照)。
    std::string shaderPath;
    // shaderPath設定時のみ意味を持つ。false=不透明(BlendEnable=FALSE)、true=SrcAlpha/InvSrcAlphaブレンド。
    bool shaderAlphaBlend = false;
    // カスタムシェーダーへ渡す汎用の進捗/強度値(0..1等、意味はシェーダー依存)。Lua `scene:setSpriteEffect`で
    // 実行時に書き換え可能(頂点ごとに補間されるので同一バッチ内でもスプライト単位に異なる値を渡せる)。
    float effectValue = 0.0f;
    // カスタムシェーダーへ渡す汎用パラメーター4つ(意味はシェーダー依存)。effectValue と同じく
    // 頂点属性(TEXCOORD2)として補間される = バッチを壊さず毎フレーム安価。HLSL側は VSIn/PSIn に
    // `float4 params : TEXCOORD2;` を足して読む(docs/AUTHORING.md 参照)。Lua `scene:setSpriteParams`。
    DirectX::XMFLOAT4 shaderParams{0.0f, 0.0f, 0.0f, 0.0f};

    // --- フリップブックアニメ（スプライトシート）。animFrames>0 で有効になり、テクスチャを
    // animCols x N グリッドとみなして frame = floor(t*animFps) % animFrames のセルを自動で
    // uvMin/uvMax に設定する(ユーザー指定の uvMin/uvMax は無視)。Editor 中もプレビュー再生。
    // UV 計算は renderer/SpriteAnim.h の純関数(単体テスト対象)。Lua `scene:setSpriteAnim`。
    int   animFrames = 0;      // 総フレーム数(0=アニメなし)
    float animFps    = 8.0f;   // 再生速度(フレーム/秒)
    int   animCols   = 0;      // シートの列数(0=animFramesと同じ=横1行ストリップ)
    int   animRow    = 0;      // シート内の開始行(複数アニメを行で並べたシート用)
    int   animRows   = 0;      // シートの総行数(0=自動: animRow + ceil(animFrames/animCols))
    int   animMode   = 0;      // 0=ループ 1=単発(最終フレームで停止) 2=往復(ピンポン)
    // 再生位置(秒。非シリアライズ)。Application::Update が毎フレーム dt を足す。
    // Lua `scene:setSpriteAnim` / `scene:restartSpriteAnim` で 0 に戻すと頭から再生される
    float _animT = 0.0f;

    // --- UVスクロール(単位/秒)。時間経過で uvMin/uvMax の両方へ加算(溶岩表面・滝・背景ループ用)。
    // animFrames>0 のときはフリップブック優先で無視。Lua `scene:setSpriteScroll`。
    float scrollU = 0.0f;
    float scrollV = 0.0f;
};

// --- ゲーム内UI（retained-mode、Unity uGUI / Godot Control 相当）---
// UICanvas をルートに、その子孫エンティティへ UIRect（レイアウト）＋ UIImage/UIText/UIButton
// （見た目/挙動）を付けて構成する。親子関係は Transform::parent（既存の階層機構）を使う。
// データのみ。レイアウト解決/描画/入力は UISystem が Play 中に駆動する。

// UIキャンバス: UIツリーのルート。これが付いたエンティティの子孫がUI要素になる
struct UICanvas
{
    float refWidth  = 1920.0f;   // 基準解像度
    float refHeight = 1080.0f;
    int   scaleMode = 0;         // 0=ScaleToFit(等比縮放・中央寄せレターボックス) 1=ConstantPixel(左上原点実ピクセル)
                                 // 2=StretchToFill(縦横を個別倍率で引き伸ばし・余白なし。装飾量は小さい方の倍率)
                                 // 2=StretchToFill(縦横個別倍率でビューポート全体に敷き詰め・余白なし)
    int   sortOrder = 0;         // キャンバス間の描画順(小→大)
    bool  visible   = true;
};

// UI矩形: レイアウトノード(UnityのRectTransform相当)。全UI要素に必須。
// 解決式（親矩形からの実ピクセル矩形の求め方）:
//   rectMin = parentMin + parentSize*anchorMin + offsetMin
//   rectMax = parentMin + parentSize*anchorMax + offsetMax
// Unity 同様、常に offsetMin/offsetMax(px) で保持する
// (アンカー一致時は offsetMin=pos-size*pivot, offsetMax=pos+size*(1-pivot) に相当)。
struct UIRect
{
    DirectX::XMFLOAT2 anchorMin{0.5f, 0.5f};  // 親矩形内の正規化アンカー
    DirectX::XMFLOAT2 anchorMax{0.5f, 0.5f};
    DirectX::XMFLOAT2 pivot{0.5f, 0.5f};
    DirectX::XMFLOAT2 offsetMin{-50.0f, -50.0f};
    DirectX::XMFLOAT2 offsetMax{50.0f, 50.0f};
    bool visible = true;   // false なら自分と子孫を描画しない
    // 兄弟間の並び順(小さいほど先に描く = 奥)。UIエディタの階層ツリーの D&D 並べ替えが
    // 書き換える。同値は registry 走査順を維持(stable)。UIRect を持たない中間ノードは 0 扱い。
    int order = 0;
    // 視覚回転(度、時計回り正)。レイアウト解決後に pivot 点回りへ掛かり、子孫にも階層合成で
    // 効く（斜め配置パネル・傾けた強調テキスト用）。レイアウト自体は軸平行のまま。
    float rotation = 0.0f;
    // 横スキュー(度)。平行四辺形のバナー/ボタン用。UIScrollView ノード自身では rotation と
    // ともに無視される（軸平行クリップと両立しないため）。
    float skewX = 0.0f;
    // 子ツリーを自分の解決済み矩形でクリップ(マスク)する。ワイプ公開・マーキー・
    // ゲージ内スクロール等の「はみ出しを隠す」表現用。UIScrollView と同じ軸平行 GPU
    // シザーなので、このノード自身の rotation/skewX は無視される(ScrollView と同じ制限)。
    bool clipChildren = false;

    // 視覚スケール(pivot 点回り)。rotation/skewX と同じくレイアウト矩形は変えず、自分と子孫の
    // 見た目だけを拡縮する。Unity RectTransform の localScale 相当。
    // UIAnimator/UiTween の実行時スケール(_curScale*)とは**掛け算**で合流するので、
    // 「静的に 0.8 倍で置いた要素」にポップ演出を足しても互いを壊さない。
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    // 視覚アルファ。自分と子孫にまとめて掛かる(グループ透過)。UIImage/UIText の color.w とは
    // 別枠 = 「パネルごとフェード」と「この文字だけ薄く」を独立に指定できる。
    float alpha = 1.0f;
};

// UI画像(または単色矩形)
struct UIImage
{
    std::string texturePath;                    // assets相対。空=単色塗り矩形
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT2 uvMin{0.0f, 0.0f};        // アトラス切り出し
    DirectX::XMFLOAT2 uvMax{1.0f, 1.0f};        // uv範囲が[0,1]を超えるとタイル繰り返し(内部でクワッド分割描画)
    DirectX::XMFLOAT4 sliceBorder{0.0f, 0.0f, 0.0f, 0.0f}; // 9-slice境界px(左,上,右,下)。全0で無効
    float cornerRadius = 0.0f;                  // 単色矩形時のみ有効
    bool raycastBlock = true;                   // 手前に描かれた自分がクリックを遮る(UnityのraycastTarget相当)
    float fillAmount = 1.0f;                    // 表示割合0..1(HPバー/ゲージ用。1=全表示、0=非表示)
    int fillDir = 0;                            // fillが増える方向 0=左から 1=右から 2=下から 3=上から
                                                // 4=放射・時計回り 5=放射・反時計回り(クールダウン円)
    float fillOrigin = 0.0f;                    // 放射fill(fillDir>=4)の開始角(度。0=真上、時計回り正)
    // 形状。0以外は cornerRadius/9-slice を無視。テクスチャは矩形と同じ UV をポリゴンで
    // 切り抜いて貼る(=どんな画像も形にマスクされる。丸アイコン等)。リングだけ単色専用
    int shape = 0;                              // 0=矩形 1=楕円 2=リング(枠円) 3=ダイヤ 4=六角形 5=三角形(上向き)
    float ringThickness = 8.0f;                 // shape=2 の帯の太さ(キャンバスpx)
    // UVスクロール(uv/秒)。タイル繰り返し(uvMax>1)と併用で流れるストライプ/警告帯/
    // コンベアなどのパターンアニメになる。9-slice と shape!=0 では無効
    DirectX::XMFLOAT2 uvScroll{0.0f, 0.0f};
    // --- 連番アニメ(スプライトシート)。animFrames>0 で有効。テクスチャを animCols x N の
    // グリッドとみなして frame = floor(t*animFps) のセルを uvMin/uvMax の代わりに使う
    // (ユーザー指定の uvMin/uvMax と uvScroll は無視される)。Sprite2D と同じ UV 計算
    // (renderer/SpriteAnim.h の純関数)。エディタ中もプレビュー再生する。
    // 9-slice / shape!=0 では無効(UV を矩形前提で扱うため)。Lua `scene:setUiAnim`
    int   animFrames = 0;      // 総フレーム数(0=アニメなし)
    float animFps    = 8.0f;   // 再生速度(フレーム/秒)
    int   animCols   = 0;      // シートの列数(0=animFramesと同じ=横1行ストリップ)
    int   animRow    = 0;      // シート内の開始行(複数アニメを行で並べたシート用)
    int   animRows   = 0;      // シートの総行数(0=自動: animRow + ceil(animFrames/animCols))
    int   animMode   = 0;      // 0=ループ 1=単発(最終フレームで停止) 2=往復(ピンポン)
    // 単発/往復で「いつ再生を始めたか」を持つランタイム値(秒。非シリアライズ)。
    // Lua `scene:setUiAnim` / `scene:restartUiAnim` で 0 に戻すと頭から再生される
    float _animT = 0.0f;
    // グラデーション: color から gradientColor2 へ線形補間(頂点シェード=テクスチャ/9-slice/角丸
    // 全対応・回転追従)。gradientColor2 のアルファは無視される(AA フリンジ保護のため)
    int gradientDir = 0;                        // 0=なし 1=横(左→右) 2=縦(上→下) 3=斜め(左上→右下) 4=放射(中心→外)
    DirectX::XMFLOAT4 gradientColor2{1.0f, 1.0f, 1.0f, 1.0f};
    // グロススイープ: gradientDir の方向に gradientColor2 の光帯が周期的に流れる(ガチャボタンの
    // 光沢流し)。周回数/秒。0=静的グラデ(通常の線形補間)。gradientDir=0/4 なら無効
    float gradientScrollSpeed = 0.0f;
    // 縁取り(枠線)。0=無効。角丸に追従する
    float outlineWidth = 0.0f;                  // キャンバスpx
    DirectX::XMFLOAT4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};
    // 枠線スタイル(shape=0 の矩形専用。他形状は実線)。1/2 は cornerRadius を無視
    int outlineStyle = 0;                       // 0=実線 1=破線 2=コーナーブラケット(四隅の鉤括弧。SF HUD 定番)
    float outlineDash = 12.0f;                  // 破線=1区切りの長さ / ブラケット=腕の長さ(キャンバスpx)
    // 分割ゲージ: fill 軸と直交する区切り線を(segments-1)本重ね描きして「n分割チャンク」に
    // 見せる(スタミナ/弾数ゲージ)。0=無効。矩形+線形fill(fillDir 0-3)専用
    int segments = 0;
    float segmentGap = 3.0f;                    // 区切り線の太さ(キャンバスpx)
    DirectX::XMFLOAT4 segmentColor{0.0f, 0.0f, 0.0f, 0.7f};
    // ドロップシャドウ(矩形近似=テクスチャのアルファ形状は反映しない)。α=0 で無効
    DirectX::XMFLOAT4 shadowColor{0.0f, 0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT2 shadowOffset{2.0f, 2.0f}; // キャンバスpx
    float shadowSoftness = 4.0f;                // ぼかし広がり(キャンバスpx)。0=シャープ
};

// UIテキスト
struct UIText
{
    std::string text = "テキスト";
    float fontSize = 24.0f;
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
    int alignH = 1;      // 0=左 1=中央 2=右
    int alignV = 1;      // 0=上 1=中央 2=下
    bool wrap = false;   // 矩形幅で折り返し
    // 縁取り(8方位の重ね描き)。0=無効。ゲームUIの可読性の要
    float outlineWidth = 0.0f;                  // キャンバスpx
    DirectX::XMFLOAT4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};
    // 影(1オフセットの先描き)。α=0 で無効
    DirectX::XMFLOAT4 shadowColor{0.0f, 0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT2 shadowOffset{1.0f, 1.0f}; // キャンバスpx
    // カスタムフォント(assets相対 .ttf/.otf。空=既定のYu Gothic)。ゲームの「らしさ」の要。
    // 動的フォントアトラスで任意サイズにシャープ。ロード失敗は警告+既定フォントへフォールバック
    std::string fontPath;
    // タイプライター表示(文字/秒)。0=無効(即全表示)。Play 中のみ1文字ずつ現れる(UTF-8 の
    // コードポイント単位=日本語も1文字ずつ)。setUiText で文字列を変えると先頭から再生し直す。
    // 整列は全文のサイズで固定(表示が進んでも文字がずれない)
    float typewriterSpeed = 0.0f;
    // 字間(キャンバスpx。負=詰める)。0以外は1文字ずつ描く per-glyph モードになる
    // (wrap とは非両立=wrap 優先で無効化。改行 \n は対応)
    float letterSpacing = 0.0f;
    // 文字アニメ(per-glyph モード)。ゆらぎ系のにぎやかしテキスト(Godot RichTextLabel の
    // [wave][shake][rainbow] 相当)。wrap=true では無効
    int charAnim = 0;                           // 0=なし 1=ウェーブ(上下うねり) 2=ジッター(ガタガタ震え) 3=レインボー(色相回転)
    float charAnimAmount = 4.0f;                // ウェーブ/ジッターの振幅(キャンバスpx)
    float charAnimSpeed = 2.0f;                 // 周波数(Hz)。レインボーは色相周回/秒×0.25
    // テキストグラデーション(本体のみ。縁取り/影には掛からない)。α は無視
    int gradientDir = 0;                        // 0=なし 1=横(左→右) 2=縦(上→下)。金色タイトル等
    DirectX::XMFLOAT4 gradientColor2{1.0f, 1.0f, 1.0f, 1.0f};
    // リッチテキスト(インラインタグ。明示オプトイン)。true でテキスト内の
    // [c=RRGGBB]..[/c](色スパン) / [wave][shake][rainbow]..[/同](スパン単位の文字アニメ。
    // 振幅/速度は charAnimAmount/Speed を流用)を解釈する。入れ子なし・閉じ忘れは文末まで・
    // 不正/未知のタグはそのまま表示。per-glyph 描画を強制(wrap=true では無効=wrap 優先)。
    // 整列・タイプライターの文字数はタグ除去後で数える。gradientDir は rich では無効
    bool rich = false;

    // ランタイム専有（非シリアライズ）: タイプライターの経過秒
    float _twT = 0.0f;
};

// UIボタン(同一エンティティの UIImage を状態色でティントする)
struct UIButton
{
    std::string onClickEvent;                     // クリック時に events へ emit するイベント名。空=無効
    DirectX::XMFLOAT4 normalColor{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 hoverColor{0.85f, 0.85f, 0.85f, 1.0f};
    DirectX::XMFLOAT4 pressedColor{0.65f, 0.65f, 0.65f, 1.0f};
    bool interactable = true;
    // 効果音(AudioSource::clipPath と同じ assets 相対 wav。空=鳴らさない)。
    // hover はカーソル/フォーカスが乗った瞬間、click はクリック確定(release-inside)時に 1 回再生。
    std::string hoverSound;
    std::string clickSound;

    // ランタイム専有（非シリアライズ）
    bool _hovered = false;
    bool _pressed = false;
};

// UIスライダー: 自分の UIRect 全体が操作域。トラック(角丸バー)+塗り+つまみ(円)を自前描画する
// (UIImage 不要)。クリック/ドラッグで value を変更し、値が変わったフレームだけ onChangeEvent を
// emit する(イベントの data.value に実値が載る)。Lua: scene:getUiSlider / setUiSlider。
struct UISlider
{
    float value    = 0.5f;   // 実値(minValue..maxValue)。0..1 正規化ではない
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step     = 0.0f;   // 0=連続。>0 なら実値をこの刻みへスナップ(0.1 など)
    std::string onChangeEvent;   // 値が変わった時に events へ emit するイベント名。空=無効
    DirectX::XMFLOAT4 trackColor{0.22f, 0.22f, 0.27f, 1.0f};
    DirectX::XMFLOAT4 fillColor{0.30f, 0.55f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 knobColor{1.0f, 1.0f, 1.0f, 1.0f};
    bool interactable = true;

    // ランタイム専有（非シリアライズ）
    bool _hovered  = false;
    bool _dragging = false;
};

// UIトグル(チェックボックス): 自分の UIRect 全体が箱＝クリック域。isOn で内側に塗りが入る。
// クリック確定(release-inside)で isOn が反転し onChangeEvent を emit(data.value = 1/0)。
// ラベルは子の UIText で添える(ボタンのラベルと同じ流儀)。Lua: scene:getUiToggle / setUiToggle。
struct UIToggle
{
    bool isOn = false;
    std::string onChangeEvent;
    DirectX::XMFLOAT4 boxColor{0.22f, 0.22f, 0.27f, 1.0f};
    DirectX::XMFLOAT4 checkColor{0.30f, 0.55f, 1.0f, 1.0f};
    bool interactable = true;

    // ランタイム専有（非シリアライズ）
    bool _hovered = false;
    bool _pressed = false;
};

// UIスクロールビュー: 自分の UIRect がビューポート(クリップ枠)。子ツリーはこの矩形で
// クリップされ、scrollX/scrollY(キャンバス空間 px)ぶんずれて描かれる。コンテンツの大きさは
// 子の解決済み矩形の合併から毎フレーム自動計測され、スクロールは 0..(コンテンツ−ビュー) に
// クランプされる。操作はホバー中のマウスホイール(縦。Shift なし横は horizontal のみの時)と、
// ドラッグ/フリック(慣性)スクロール(dragScroll。Play 中のみ): ビューポート内で押して
// スクリーン 6px を超えて動いたらドラッグ確定=指に張り付いてスクロールし、離すと直近速度が
// flickDecay の指数減衰で流れる。確定した瞬間に進行中のボタン/トグル押下・スライダードラッグは
// キャンセルされる(リスト内ボタンをドラッグしてもクリック誤発火しない。タッチUIの定番挙動)。
// はみ出た子はクリックも効かない(レイキャスト矩形もクリップ)。
struct UIScrollView
{
    bool  vertical   = true;    // 縦スクロールを許可
    bool  horizontal = false;   // 横スクロールを許可
    float scrollX = 0.0f;       // 現在のスクロール量(キャンバス空間 px。0=先頭)
    float scrollY = 0.0f;
    float wheelSpeed = 48.0f;   // ホイール1ノッチのスクロール量(px)
    bool  showBar = true;       // スクロールバー(位置インジケータ)を描く
    DirectX::XMFLOAT4 barColor{1.0f, 1.0f, 1.0f, 0.35f};
    bool  dragScroll = true;    // ドラッグ/フリックスクロールを許可
    float flickDecay = 4.0f;    // フリック慣性の指数減衰率(/秒)。0=慣性なし(離した瞬間停止)

    // ランタイム専有（非シリアライズ）: 前フレームで計測したコンテンツサイズ(キャンバス px)
    float _contentW = 0.0f;
    float _contentH = 0.0f;
    // ランタイム専有（非シリアライズ）: ドラッグ/フリックの進行状態
    bool  _dragging  = false;   // ドラッグ候補〜確定中(mouseDown 継続中。ビュー外へ出ても追従)
    bool  _dragMoved = false;   // しきい値(スクリーン6px)超過済み=ドラッグ確定
    float _prevMX = 0.0f;       // 前フレームのマウス位置(スクリーンpx。確定までは押下開始点)
    float _prevMY = 0.0f;
    float _velX = 0.0f;         // フリック慣性速度(キャンバスpx/秒。ドラッグ中は平滑追跡)
    float _velY = 0.0f;
    float _scale = 1.0f;        // キャンバス→スクリーン倍率(トラバース時に保存。px換算用)
};

// UIレイアウトコンテナ: 直下の子(UIRect 持ち)へ「セル矩形」を順番に配り、子はそのセルを
// 親矩形としてアンカー解決する(セル内で anchorMin=Max=中央+サイズ指定でも、0-1 ストレッチ
// でも、子の書式はそのまま効く)。手動 offset 計算なしでリスト/ツールバー/グリッドが組める。
// セルは自分の解決済み矩形の内側(padding 控除)に padding 起点で敷き詰める。
// メニュー項目列・インベントリグリッド・スキルバー等の定番骨格用。UIScrollView と併用可
// (コンテンツ側ノードに付ける)。
struct UILayout
{
    int   mode  = 0;         // 0=縦積み(VBox) 1=横並び(HBox) 2=グリッド(左上から行優先)
    float cellW = 200.0f;    // セル幅(キャンバスpx)。VBox では 0=親の内側いっぱい
    float cellH = 60.0f;     // セル高(キャンバスpx)。HBox では 0=親の内側いっぱい
    float spacing = 8.0f;    // セル間の間隔(px)
    DirectX::XMFLOAT4 padding{0.0f, 0.0f, 0.0f, 0.0f};   // 内側余白(左,上,右,下 px)
    int   gridCols = 4;      // mode=2 の列数(1以上)
};

// UI アニメーション（UIRect 持ちエンティティに付ける。Play / ゲームモード中のみ再生）。
// 出現アニメ（Play 開始時と Lua scene:showUi() で再生）・ホバー/押下スケール（要 UIButton）・
// ループアニメの 3 系統。視覚効果（平行移動/拡縮/アルファ）は自分と子孫にまとめて掛かる。
// イージング(showEasing): 0=リニア 1=イーズイン 2=イーズアウト 3=イン/アウト 4=バック(勢い)
//                         5=バウンス 6=弾性 7=エクスポ(鋭く減速) 8=インバック(溜めて発進)
//                         9=イン/アウトバック 10=クイント(強い減速) 11=サイン(ゆったり)
struct UIAnimator
{
    // 出現アニメ
    int  showAnim     = 1;      // 0=なし 1=フェード 2=ポップ(拡大) 3=左から 4=右から 5=上から 6=下から 7=スピン(回転入場)
                                // 8=バウンド落下 9=フリップ(縦つぶれから開く) 10=シェイク入場 11=横フリップ(扉/カード)
                                // 8=バウンド落下(上から弾んで着地) 9=フリップ(縦つぶれから開く) 10=シェイク入場
                                // 11=横フリップ(横つぶれから開く=扉/カード)
    f32  showDuration = 0.35f;  // 秒
    f32  showDelay    = 0.0f;   // 秒（複数要素を順番に出すのに使う）
    int  showEasing   = 2;      // 既定 = イーズアウト
    f32  slideOffset  = 80.0f;  // スライド距離（キャンバス px）

    // ホバー / 押下スケール（同一エンティティに UIButton がある時のみ効く）
    f32  hoverScale = 1.05f;
    f32  pressScale = 0.95f;
    f32  hoverSpeed = 14.0f;    // 追従速度（大きいほどキビキビ）

    // ループアニメ（常時）
    int  loopAnim   = 0;        // 0=なし 1=浮遊(上下) 2=パルス(拡縮) 3=点滅(アルファ) 4=スピン(連続回転) 5=スウィング(揺れ)
    f32  loopSpeed  = 1.0f;     // 周波数（Hz）。スピンは回転数/秒
    f32  loopAmount = 8.0f;     // 浮遊=px / パルス・点滅=割合（0.05 = ±5%）/ スウィング=±度

    // ランタイム専有（非シリアライズ・meta 未登録）
    f32  _t      = 0.0f;        // show/hide の経過秒
    int  _mode   = 0;           // 0=未開始 1=show中 2=表示済み 3=hide中 4=非表示
    f32  _hoverS = 1.0f;        // ホバースケールの平滑値
    f32  _loopT  = 0.0f;        // ループ位相（秒）
    // 今フレームの合成結果（UISystem の更新が書き、描画が読む）。スケールは X/Y 別
    // （フリップ=縦つぶれ等の非等方アニメ用。等方アニメは両方へ同値が入る）
    f32  _curScaleX = 1.0f;
    f32  _curScaleY = 1.0f;
    f32  _curAlpha = 1.0f;
    f32  _curRot   = 0.0f;      // 追加回転（度）。UIRect.rotation へ加算合成される
    DirectX::XMFLOAT2 _curOffset{0.0f, 0.0f};
};

// Lua scene:tweenUi() の 1 本ぶんの実行状態（ランタイム専用・非シリアライズ・エディタ非表示）
struct UiTween
{
    f32  t = 0.0f;              // 経過秒（delay 含む）
    f32  duration = 0.3f;
    f32  delay    = 0.0f;
    int  easing   = 2;          // UIAnimator::showEasing と同じ列挙
    bool started  = false;      // delay 明けに from 値を捕捉済みか

    bool hasMove = false;       // UIRect offset への相対移動（キャンバス px）
    DirectX::XMFLOAT2 moveDelta{0.0f, 0.0f};
    DirectX::XMFLOAT2 baseOffMin{0.0f, 0.0f};
    DirectX::XMFLOAT2 baseOffMax{0.0f, 0.0f};

    bool hasScaleX = false;     // 視覚スケール（レイアウトは変えない）。X/Y 別
                                // （scale= は両方、scaleX=/scaleY= は片方だけを動かす）
    f32  scaleXFrom = 1.0f, scaleXTo = 1.0f;
    bool hasScaleY = false;
    f32  scaleYFrom = 1.0f, scaleYTo = 1.0f;
    bool hasAlpha = false;      // 視覚アルファ乗数
    f32  alphaFrom = 1.0f, alphaTo = 1.0f;
    bool hasRotate = false;     // 視覚回転（度、絶対目標値。UIRect.rotation へ加算合成）
    f32  rotFrom = 0.0f, rotTo = 0.0f;
    bool hasColor = false;      // 視覚カラー乗数（RGB。1 超えも許可=白フラッシュ用。α は alpha= で）
    DirectX::XMFLOAT3 colFrom{1.0f, 1.0f, 1.0f}, colTo{1.0f, 1.0f, 1.0f};
    bool hasShake = false;      // 振動（減衰付き）。amplitude=px、freq=Hz。duration で 0 に減衰
    f32  shakeAmp  = 8.0f;
    f32  shakeFreq = 24.0f;
    bool hasFill = false;       // UIImage.fillAmount を絶対目標値へ(ゲージのなめらか増減。
                                // from は開始時の現在値を捕捉=ゴーストバー遅延削れが作れる)
    f32  fillFrom = 1.0f, fillTo = 1.0f;
    bool hasCount = false;      // UIText へ数字ロール(カウントアップ演出)。from 省略時は
                                // 開始時のテキストを数値解釈(失敗=0)=連続加算が自然に繋がる
    bool   countFromSet = false;
    double countFrom = 0.0, countTo = 0.0;
    std::string countFmt;       // printf 書式(既定 "%d"。"%05d"=ゼロ埋め、"%.1f"=小数)
    // 完了コールバック(SE 同期・演出チェーン用)。tween 完了フレームの更新後に 1 回呼ばれる
    std::function<void()> onComplete;
};
struct UITweenState
{
    std::vector<UiTween> tweens;
    f32 visScaleX = 1.0f;       // tween 完了後も持続する視覚スケール / アルファ / 回転 / カラー
    f32 visScaleY = 1.0f;
    f32 visAlpha = 1.0f;
    f32 visRot   = 0.0f;
    DirectX::XMFLOAT3 visColor{1.0f, 1.0f, 1.0f};
    // 今フレームの評価結果（UISystem の更新が書き、描画が読む）
    f32 _curScaleX = 1.0f;
    f32 _curScaleY = 1.0f;
    f32 _curAlpha = 1.0f;
    f32 _curRot   = 0.0f;
    DirectX::XMFLOAT3 _curColor{1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT2 _curOffset{0.0f, 0.0f};   // shake の今フレームぶん（減衰済み）
};

// タイムラインで作った UI アニメクリップ（.uianim）の再生器。
// UIAnimator が「決め打ちのプリセット」なのに対し、こっちは AnimationEditorPanel で
// キーを打って作る任意アニメ。1 エンティティに付けると、そのサブツリー全体を
// トラックの target（名前パス）で指して動かせる = パネル1個に対して再生器1個で済む。
//
// UIAnimator / UiTween との合成規約:
//   - offset/anchor/rotation/色/fill など「コンポーネント値そのもの」を持つトラックは**直接書き込む**。
//     エディタのプレビュー（RenderPreview）でもスクラブが効くのはこのため。
//   - scale / groupAlpha だけは視覚合成なので _cur* に出し、UIAnimator/UiTween と掛け算で合流する。
struct UIAnimPlayer
{
    std::string clipPath;          // assets 相対（例 "uianim/menu_open.uianim"）。空 = 何もしない
    bool  playOnStart = true;      // Play 開始時に自動再生
    bool  loop        = false;     // クリップ側の loop を上書きしたい時に true（false ならクリップ設定に従う）
    f32   speed       = 1.0f;      // 再生速度倍率（負値で逆再生、0 で一時停止）
    std::string finishEvent;       // 再生完了時に EventBus へ発火（空 = 発火しない）

    // ランタイム専有（非シリアライズ・meta 未登録）
    f32  _time     = 0.0f;         // クリップ内の再生位置（秒）
    f32  _prevTime = 0.0f;         // 前フレームの位置（イベント発火の区間判定用）
    bool _playing  = false;
    bool _finished = false;        // 単発クリップが終端に達したか（finishEvent の二重発火防止）
    bool _loaded   = false;        // clipPath のロードを試したか（毎フレームの再ロードを防ぐ）
    // 今フレームの視覚合成結果（クリップ評価が書き、描画が読む）
    f32  _curScaleX = 1.0f;
    f32  _curScaleY = 1.0f;
    f32  _curAlpha  = 1.0f;
};

// スプライトシート（.spranim）の連番アニメ再生器。同じエンティティの Sprite2D または
// UIImage の uvMin/uvMax を毎フレーム書き換える（両方あれば両方に書く）。
// Sprite2D::animFrames / UIImage::animFrames の「等分割・連続フレーム」より表現力が高い
// （任意セル順・可変フレーム長・名前付きシーケンス切替）。両方設定した場合はこちらが勝つ。
struct SpriteAnimator
{
    std::string sheetPath;         // assets 相対（例 "spriteanim/player.spranim"）。空 = 何もしない
    std::string currentSeq;        // 再生中のシーケンス名（空 = シートの先頭）
    bool  playOnStart = true;
    f32   speed       = 1.0f;
    bool  applyTexture = true;     // シートの texture を Sprite2D/UIImage の texturePath へも反映するか

    // ランタイム専有（非シリアライズ・meta 未登録）
    f32  _time     = 0.0f;
    bool _playing  = false;
    bool _finished = false;        // 単発シーケンスの完了（finishEvent の二重発火防止）
    bool _loaded   = false;
    i32  _curFrame = -1;           // 今フレームのセル番号（エディタ表示用。-1 = 未評価）
};

// プレハブインスタンスと元 .prefab の紐付け（インスタンスの**ルートだけ**が持つ）。
// これがあると Inspector に「適用 / 元に戻す / 変更点」が出て、Unity のプレハブと同じ
// 往復編集ができる。持っていないエンティティは今まで通りの「展開しっぱなしのコピー」。
//
// 子エンティティには付けない。理由: サブツリー全体でひとつのインスタンスなので、
// 子ごとにリンクがあると「どこが起点か」が曖昧になり、入れ子プレハブと区別できなくなる。
struct PrefabLink
{
    std::string sourcePath;   // assets 相対（例 "prefabs/ui/hp_bar.prefab"）
};

// 3D 空間オーディオ音源。Transform のワールド位置がエミッタになる。
struct AudioSource
{
    std::string clipPath;             // assets 相対（例 "audio/sfx/foot.wav"）
    f32  volume      = 1.0f;
    bool loop        = false;
    bool spatial     = true;          // false なら 2D SE
    bool playOnStart = true;          // Play 開始時に自動再生
    f32  minDistance = 1.0f;
    f32  maxDistance = 30.0f;

    // ランタイム専有（非シリアライズ）
    i32  runtimeSlot     = -1;
    bool startedThisPlay = false;
};

// ステージギミック。Transform を基準位置として、時間で動く/塞ぐ「ステージ部品」を表す。
// 実際の動き（周期・早送り・ワールドクロック）はゲームスクリプト(Lua)が解釈して駆動する＝データのみ。
// エディタで配置・パラメータ設定して、scene:gimmicks() で Lua から読む。
enum class GimmickKind : uint8_t
{
    StaticWall = 0,  // 動かない。常に塞ぐ（外周の壁など）
    SpikePulse = 1,  // Y方向に上下（サイン波）。せり上がってる時だけ塞ぐ。deadly なら直撃死
    SlideX     = 2,  // X方向に往復スライド。常に塞ぐ（動く壁）
    SlideZ     = 3,  // Z方向に往復スライド。常に塞ぐ（動く床/トロッコ）
};

struct Gimmick
{
    int  kind      = 0;      // GimmickKind（0=StaticWall,1=SpikePulse,2=SlideX,3=SlideZ）
    f32  period    = 4.0f;   // 1周期の秒数
    f32  phase     = 0.0f;   // 位相オフセット（0..1）
    f32  amplitude = 1.6f;   // 動く量（SpikePulse=せり上がり高 / Slide=振幅）
    f32  threshold = 0.5f;   // SpikePulse: この正規化高さ(0..1)以上で塞ぐ
    bool solid     = true;   // 当たり判定を持つか
    bool deadly    = false;  // SpikePulse: せり上がりで直撃死するか
};

// --- Physics Components ---

static constexpr uint32_t kInvalidBodyId = 0xFFFFFFFF;

enum class MotionType : uint8_t
{
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2,
};

struct RigidBody
{
    MotionType motionType    = MotionType::Dynamic;
    f32        mass          = 1.0f;
    f32        restitution   = 0.4f;   // 適度に弾む
    f32        friction      = 0.3f;   // 低め → 滑りやすく不安定に
    f32        linearDamping  = 0.02f;  // 移動減衰を弱く
    f32        angularDamping = 0.01f;  // 回転減衰を弱く → 倒れやすい
    bool       useGravity    = true;
    // 連続衝突判定(CCD)。既定 false = Jolt の Discrete。
    // Discrete は「1 ステップぶん移動した後の位置」でしか当たりを見ないので、
    // 1 フレームで自分の厚みより長く動く物（弾丸・投擲物・高速の乗り物）は
    // 薄い壁をすり抜ける。true にすると LinearCast（移動線分で判定）になる。
    // 常時 true にはしない: LinearCast は明確にコストが高く、普通の箱や瓦礫には不要。
    bool       continuousCollision = false;

    // PhysicsSystem が管理（ユーザーは触らない）
    uint32_t   bodyId = kInvalidBodyId;
};

struct BoxCollider
{
    DirectX::XMFLOAT3 halfExtents = {0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT3 offset      = {0.0f, 0.0f, 0.0f};
};

struct SphereCollider
{
    f32               radius = 0.5f;
    DirectX::XMFLOAT3 offset = {0.0f, 0.0f, 0.0f};
};

struct CapsuleCollider
{
    f32               radius     = 0.5f;
    f32               halfHeight = 1.0f;
    DirectX::XMFLOAT3 offset     = {0.0f, 0.0f, 0.0f};
};

struct ConvexHullCollider
{
    std::vector<DirectX::XMFLOAT3> points; // スケール適用済みワールド頂点
    DirectX::XMFLOAT3 offset = {0.0f, 0.0f, 0.0f};
};

// --- Character Controller（Jolt CharacterVirtual ベースのキャラ移動部品）---
// カプセル形状で collide-and-slide / 接地判定 / 段差 / 斜面登坂を行う、
// プレイヤー/敵キャラ用の移動コンポーネント。RigidBody とは排他（同時付与禁止）。
// 実体（JPH::CharacterVirtual）は PhysicsSystem が Play 中だけ生成・保持する。
// Lua から physics:move(e,x,z) / physics:jump(e,v) / physics:isGrounded(e) で駆動する。
struct CharacterController
{
    // ---- 形状（カプセル）----
    f32 radius      = 0.4f;   // カプセル半径
    f32 halfHeight  = 0.6f;   // カプセル円柱部の半分の高さ（全高 = 2*(halfHeight+radius)）
    DirectX::XMFLOAT3 offset = {0.0f, 0.0f, 0.0f}; // Transform 原点に対する形状ローカルオフセット

    // ---- 移動パラメータ ----
    f32  mass          = 70.0f;  // 質量(kg)。乗っている剛体を押す力に使う
    f32  maxSlopeDeg   = 50.0f;  // これ以上の傾斜は歩いて登れない（度。Joltへはradian変換）
    f32  stepHeight    = 0.3f;   // 登れる段差の高さ（ExtendedUpdateSettings.mWalkStairsStepUp に反映）
    f32  jumpSpeed     = 6.0f;   // jump() 既定の初速（Lua から上書き可）
    f32  gravityScale  = 1.0f;   // このキャラに掛かる重力倍率（PhysicsSystemの重力×scale）

    // ---- ランタイム専有（非シリアライズ・複製時に必ず無効化）----
    DirectX::XMFLOAT3 _desiredVel = {0.0f, 0.0f, 0.0f}; // move() が積む目標水平速度(world XZ)
    f32   _verticalVel = 0.0f;  // jump/落下の鉛直速度（接地でリセット）
    bool  _jumpQueued  = false; // このフレーム jump 要求があったか
    bool  _grounded    = false; // 直近 ExtendedUpdate 後の接地状態（isGrounded() が読む）
    bool  _registered  = false; // PhysicsSystem に CharacterVirtual が生成済みか
    // CharacterVirtual* はヘッダに出さない。PhysicsSystem 内 map<entity,Ref<CharacterVirtual>> で持つ。
};

// スクリプトコンポーネントが公開するプロパティの値。
// .lua の `properties = {...}` 宣言ごとに 1 つ。エディタの Inspector で編集でき、
// シーン/プレハブに保存され、Play 時に Lua スクリプトへ self.<name> として注入される。
// これにより「巨大な 1 個のコントローラ」ではなく「パラメータ付きの再利用部品」を
// エンティティに貼って数値だけ調整する Unity ライクな作り方ができる。
enum class ScriptPropType : uint8_t
{
    Float  = 0,
    Int    = 1,
    Bool   = 2,
    String = 3,
    Vec3   = 4,
    Color  = 5,
    Entity = 6,   // 他エンティティへの参照（名前で保存。Play 時に self.<name> へ Entity を注入）
};

struct ScriptProp
{
    std::string       name;
    ScriptPropType    type = ScriptPropType::Float;
    double            num  = 0.0;                // Float / Int
    bool              b    = false;              // Bool
    std::string       str;                       // String / Entity(参照先エンティティ名)
    DirectX::XMFLOAT3 vec{0.0f, 0.0f, 0.0f};     // Vec3 / Color
    // Entity 型のときだけ意味を持つ。★参照の正はこちらで、str(名前) は保存時に
    // guid から引き直される派生値。0 = 未設定（名前だけの旧データ）。
    // 詳細は Trigger::filterGuid のコメント。
    uint64_t          guid = 0;
};

struct LuaScript
{
    // シリアライズ対象
    std::string scriptPath;   // assets 相対パス（例 "scripts/player.lua"）
    bool        enabled = true;

    // スクリプトが公開するプロパティのインスタンス値（.lua の properties 宣言と対応）。
    // シリアライズ対象。スキーマ（型/既定値/範囲）は ScriptEngine が .lua から解析して保持する。
    std::vector<ScriptProp> props;

    // ランタイム専有（非シリアライズ）
    // sol::environment / sol::table を直接持つとヘッダ依存が膨らむため void で隠蔽
    std::shared_ptr<void> env;    // sol::environment
    std::shared_ptr<void> self;   // sol::table
    bool started   = false;
    bool loadError = false;
    std::string errorMessage;   // loadError=true のときの最後のエラー文字列（dx12_get_lua_component_state で返す）
};

// 配置できるパーティクル放出器（エディタで置く「エフェクト部品」）。
// 既存の即時放出パーティクル系（ParticleSystem::Emit）に毎フレーム乗せる薄いデータ。
// Transform のワールド位置から放出する。Trigger の PlayEffect/StopEffect で発火/停止できる。
// kind / blend は ParticleKind / ParticleBlend（renderer/ParticleSystem.h）の整数値。
struct ParticleEmitter
{
    int  kind        = 0;      // 見た目（0=Glow,1=Fire,2=Smoke,3=Spark,4=Magic,5=Electric,6=Ring,7=Star）
    int  blend       = 0;      // 0=加算 Additive, 1=前乗算アルファ（煙）
    int  orient      = 0;      // 粒子の向き 0=ビルボード(常にカメラ正対) 1=水平(XZ地面向き) 2=垂直(XY,+Z正対)。
                               // stretch>0 は速度整列が優先。gpu=true は非対応(常にビルボード)
    f32  rate        = 30.0f;  // 連続放出レート（個/秒）。0 で連続放出しない
    bool playOnStart = true;   // Play 開始時に自動で放出開始
    bool looping     = true;   // false なら duration 秒だけ放出して止まる（ワンショット）
    f32  duration    = 1.0f;   // looping=false のときの放出継続秒

    // 1 粒ごとの見た目（EmitParams に対応）
    DirectX::XMFLOAT3 dir{0.0f, 1.0f, 0.0f};
    f32  spread    = 0.4f;
    f32  speed     = 3.0f;
    f32  speedVar  = 0.4f;
    f32  size      = 0.3f;
    f32  sizeEnd   = 0.0f;
    f32  life      = 0.8f;
    f32  lifeVar   = 0.3f;
    DirectX::XMFLOAT3 color{1.0f, 0.6f, 0.2f};     // 開始色
    DirectX::XMFLOAT3 colorMid{1.0f, 0.6f, 0.2f};  // 中間色（hasColorMid=true の時のみ使用）
    DirectX::XMFLOAT3 colorEnd{1.0f, 0.12f, 0.05f}; // 終了色
    bool hasColorMid = false;  // true で3キー色カーブ（start→mid→end）。false なら start→end のみ
    f32  intensity = 3.0f;     // HDR 増幅（>1 で白熱→ブルーム）
    f32  gravity   = 0.0f;     // y 加速（負で落下）
    f32  drag      = 1.0f;
    f32  up        = 0.0f;     // 初速の上向きバイアス
    f32  stretch   = 0.0f;     // >0 で速度方向へ伸びる（火花/筋）
    f32  turbStrength = 0.0f;  // >0 でカールノイズ乱流（有機的な揺らぎ：煙/炎）
    f32  turbFreq     = 1.0f;  // 乱流の空間周波数
    f32  sizeMid   = -1.0f;    // >=0 で3キーサイズカーブ（start→mid→end）
    f32  distort   = 0.0f;     // >0 で歪みパーティクル（熱ゆらぎ/衝撃波。画面を歪ませる）
    bool light     = false;    // 明るい粒子上位N個を実ポイントライト化（炎が周囲を照らす）
    f32  lightRange = 3.0f;    // ポイントライト化時の到達距離
    f32  flicker      = 0.0f;  // 発光明滅の強さ（0..1）
    f32  flickerFreq  = 18.0f; // 明滅の速さ
    bool gpu       = false;    // true で GPUパーティクル（compute・最大131072・加算専用。大量粒子向け。
                               // distort/light/sizeMid/blend=α は非対応）
    std::string texturePath;  // assets 相対パス。空ならプロシージャル質感(kind依存)、指定時はテクスチャを貼る

    // ランタイム専有（非シリアライズ）
    bool _active    = true;    // 放出中か（Play 時は playOnStart で初期化。エディタは常時プレビュー）
    f32  _emitAccum = 0.0f;    // 端数の放出量を溜める
    f32  _age       = 0.0f;    // ワンショット用の経過秒
};

// 軌跡リボン（剣の残像/弾道/魔法の尾）。エンティティのワールド位置を毎フレーム記録し、
// ParticleSystem がカメラフェーシングの帯として描く。エディタでも常時プレビュー。
struct TrailRenderer
{
    bool emitting  = true;     // 位置を記録するか（OFF で尾が自然に消えていく）
    f32  width     = 0.25f;    // 帯の幅（ワールド）
    f32  life      = 0.5f;     // 各点の寿命（秒）＝帯の長さ
    DirectX::XMFLOAT3 color{0.4f, 0.8f, 1.0f};     // 先頭色
    DirectX::XMFLOAT3 colorEnd{0.1f, 0.2f, 1.0f};  // 尾の色
    f32  intensity = 2.0f;     // HDR 増幅（>1 でブルームに乗る）
    int  blend     = 0;        // 0=加算（エネルギー）, 1=前乗算アルファ（煙）
    f32  minDist   = 0.03f;    // この距離以上動いたら点を打つ
};

// 投影デカール（弾痕/血/汚れ/水たまり/落書き）。
// エンティティの Transform が作る箱（ローカル単位立方体 [-0.5,0.5] を scale で拡縮）の中に入った
// 面へ、ローカル +Y から下向きに 1 枚テクスチャを投影する。無回転で置けばそのまま床に落ちる。
//
// 方式は「クラスタードフォワードデカール」。クラスタごとのデカールリストを作り、フォワード PS の
// マテリアル確定直後に albedo/normal/roughness/metallic を書き換える（src/renderer/DecalSystem.h）。
//
// ★テクスチャは「シーン設定のアトラス 1 枚 + ここの UV 矩形」で指す。
//   コンポーネントに std::string を持たせないのは意図的な設計判断:
//   InspectorPanel の EndEdit が std::memcmp でスナップショット比較しているので、
//   std::string メンバがあると SSO 境界をまたぐ変更を取りこぼす/誤検知する。
struct DecalComponent
{
    // アトラス内のカラー矩形（UV 0..1）。(u0, v0, du, dv)。既定はアトラス全面。
    DirectX::XMFLOAT4 atlasUV{0.0f, 0.0f, 1.0f, 1.0f};
    // アトラス内の法線矩形。dv <= 0（既定）なら法線マップ無し＝受け面の法線をそのまま使う。
    DirectX::XMFLOAT4 atlasUVNormal{0.0f, 0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};   // カラーへの乗算
    f32  opacity        = 1.0f;   // 全体の不透明度
    DirectX::XMFLOAT3 emissive{0.0f, 0.0f, 0.0f};  // 加算する自己発光（HDR）
    f32  normalStrength = 1.0f;   // 法線ブレンドの強さ（atlasUVNormal がある時だけ効く）
    f32  roughness      = -1.0f;  // 受け面のラフネスを上書き。< 0 で変更しない
    f32  metallic       = -1.0f;  // 同メタリック。< 0 で変更しない
    f32  angleFadeDeg   = 60.0f;  // 投影軸と受け面法線の角度がこれを超えたら消える（引き伸ばし防止）
    f32  fadeEdge       = 0.1f;   // 箱の縁からのフェード幅（ローカル座標。0=硬い縁）
    int  sortOrder      = 0;      // 小さいものから下に重なる（同値なら不定）
};

// --- Trigger（イベント）: 範囲に入った/出た/居る ときに宣言的なアクションを実行する部品 ---
// エディタで箱/球を置き、Inspector でアクション列を組むだけで「X したら Y する」を配線できる。
// データのみ（評価は ScriptEngine::UpdateTriggers が Play 中に駆動）。JSON 保存なので Claude も書ける。
enum class TriggerShape : uint8_t { Box = 0, Sphere = 1 };
enum class TriggerWhen  : uint8_t { Enter = 0, Exit = 1, Stay = 2 };

// アクションの種類。target= 対象エンティティ名、str= シーンパス/プロパティ名/イベント名、num= 数値、vec= 移動量。
enum class TriggerActionType : uint8_t
{
    Enable      = 0,  // target の LuaScript を有効化
    Disable     = 1,  // target の LuaScript を無効化
    Destroy     = 2,  // target を削除
    Move        = 3,  // target の位置を vec だけ動かす（相対）
    PlayEffect  = 4,  // target の ParticleEmitter を放出開始
    StopEffect  = 5,  // target の ParticleEmitter を停止
    PlaySound   = 6,  // target の AudioSource を再生
    LoadScene   = 7,  // str のシーンへ即切替
    FadeToScene = 8,  // str のシーンへフェード切替（num=秒）
    SetProperty = 9,  // target の実行中スクリプトの self[str] = num
    EmitEvent   = 10, // Lua イベントバスへ events:emit(str, {value=num, target=...})
};

struct TriggerAction
{
    int  when = 0;            // TriggerWhen（Enter/Exit/Stay）
    int  type = 0;            // TriggerActionType
    std::string target;       // 対象エンティティ名（空=なし）
    // ★参照の正は guid。target(名前) は人間と git diff のための派生値で、
    //   保存時に guid から引き直すので両者はドリフトしない。0 = 未設定（名前だけの旧データ）。
    //   詳細は Trigger::filterGuid のコメント。
    uint64_t targetGuid = 0;
    std::string str;          // シーンパス / プロパティ名 / イベント名
    double num = 0.0;         // 数値パラメータ
    DirectX::XMFLOAT3 vec{0.0f, 0.0f, 0.0f}; // Move 用の移動量
};

struct Trigger
{
    int  shape = 0;                          // TriggerShape（0=Box,1=Sphere）
    DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f}; // Box 用（Transform.scale 乗算）
    f32  radius = 1.0f;                       // Sphere 用（Transform.scale 最大成分 乗算）
    DirectX::XMFLOAT3 offset{0.0f, 0.0f, 0.0f}; // 判定中心のローカルオフセット
    std::string filter;                        // 反応する対象エンティティ名（空=Player）
    // ★参照の正は guid（[[EntityGuid]]）。名前は配列位置に依存しない代わりに
    //   「リネームで切れる」「同名で曖昧になる」の 2 つが避けられない。
    //   解決順は guid → 名前 → （filter のみ）暗黙の "Player"。
    //   保存時に guid から名前を引き直すので、両者が食い違ったまま残ることはない。
    //   読み込み時に「名前 → guid」へメモリ上だけ昇格させるので、旧シーンも
    //   一度開いて保存すれば自動で移行する。0 = 未設定。
    uint64_t filterGuid = 0;
    bool once = false;                          // 一度 Enter 発火したら無効化
    std::vector<TriggerAction> actions;

    // ランタイム専有（非シリアライズ）
    bool _wasInside = false;
    bool _firedOnce = false;
};

// --- NetworkIdentity: エンティティをマルチプレイ複製対象にする印 ---
// サーバー(ホスト)が netId を採番して全クライアントへ配る。entt::entity は
// プロセスローカル(バージョン込み・join毎に変わり得る)なのでネットワークには一切流さず、
// 代わりにこの netId を安定識別子として使う。
struct NetworkIdentity
{
    f32  interestRadius  = 0.0f;   // 0 = 常に関連（距離カリングなし）。★稼働中:
                                   //   NetworkSystem.cpp:435 の IsRelevant() が今まさに送信を間引いている
    bool serverAuthority = true;   // 予約（将来のクライアント権威エンティティ用）

    // ランタイム専有（非シリアライズ）
    u32  _netId        = 0;
    u16  _owner        = 0;        // 0 = サーバー/ホスト
    bool _isLocalOwner = false;
    bool _netSpawned   = false;    // 実行時スポーン品（Stop時に破棄する対象の目印）
};

// --- NetworkTransform: Transform をスナップショット複製する設定 ---
// NetworkIdentity と併用する。サーバーは自分が権威を持つ全エンティティの位置/回転を
// 定期送信し、クライアントは受信スナップショットを補間してこのエンティティの
// Transform に書き込む。syncMode=1（オーナー予測）は自分が所有するエンティティを補間バッファに
// 積まず、NetworkSystem::RecordPredictedState → ReconcilePredictedEntity で
// サーバー確定位置と突き合わせ、ズレが snapDistance を超えたときだけ未処理入力を再適用する。
struct NetworkTransform
{
    int  syncMode       = 0;      // 0=補間(SimulatedProxy) / 1=オーナー予測(AutonomousProxy)
    f32  sendRate       = 20.0f;  // Hz（フェーズ⑧でグローバル既定と合成する予定。現状は未使用）
    bool syncPosition   = true;
    bool syncRotation   = true;
    bool syncScale      = false;
    f32  interpDelayMs  = 100.0f; // 補間バッファの遅延（ジッター吸収）
    f32  snapDistance   = 5.0f;   // これ以上の誤差はテレポート扱い。★稼働中:
                                  //   NetworkSystem.cpp:563 の reconciliation 分岐がこれで判定する
};

} // namespace dx12e
