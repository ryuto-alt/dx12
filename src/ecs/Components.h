#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e
{

class Mesh;
struct Material;
class Skeleton;
class AnimationClip;
class Animator;
class SkinningBuffer;
class NodeGraph;
class NodeAnimationClip;
class NodeAnimator;

struct NameTag
{
    std::string name;
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

struct MeshRenderer
{
    std::string modelPath; // アセット相対パス（シーン保存/読み込み用）
    std::vector<Mesh*>     meshes;
    std::vector<Material*> materials;
    std::vector<DirectX::XMFLOAT4X4> meshNodeTransforms;

    // エディタ用 PBR オーバーライド（Material ポインタに依存しない直接値）
    float overrideMetallic  = -1.0f;  // < 0 = Material の値を使う
    float overrideRoughness = -1.0f;

    // UV タイリング
    float uvScaleU = 1.0f;
    float uvScaleV = 1.0f;

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

    // インスタンシング: 共有メッシュを使う発光弾(Pfx)等は色を頂点バッファに焼かず
    // ここに持つ（setColor が書き込む）。instanced=true の間 setColor は VB を再生成しない。
    DirectX::XMFLOAT4 instanceColor = {1.0f, 1.0f, 1.0f, 1.0f};
    bool instanced = false;
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

// エディタ用グリッド床の一辺の長さ(m)。原点中心に ±kEditorGridSize/2(=250m) をカバーする。
// シェーダ(ForwardGrid.hlsl)の距離フェードはこの半分より内側で 0 になるよう調整してあるので、
// 平面のフチが矩形に途切れて見えない(無限グリッド風)。spawn/save/load で必ず同じ値を使うこと。
inline constexpr f32 kEditorGridSize = 500.0f;

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
    bool once = false;                          // 一度 Enter 発火したら無効化
    std::vector<TriggerAction> actions;

    // ランタイム専有（非シリアライズ）
    bool _wasInside = false;
    bool _firedOnce = false;
};

} // namespace dx12e
