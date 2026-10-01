#pragma once

#include <memory>
#include <cstdint>
#include <unordered_map>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"
#include "engine/core/EventBus.h"   // EngineEvent / EventBus（接触イベントの配信先）

namespace dx12e
{

struct RaycastHit
{
    bool               hit      = false;
    f32                distance = 0.0f;
    uint32_t           bodyId   = 0xFFFFFFFF;
    DirectX::XMFLOAT3  point    = {};
    DirectX::XMFLOAT3  normal   = {};
    // インスタンス群（InstanceGroup）のどのインスタンスに当たったか（サブシェイプ ID の userData）。0xFFFFFFFF = 群ではない。
    // bodyId→entity は群のエンティティを返す。
    uint32_t           instanceIndex = 0xFFFFFFFF;
    // 当たった相手のエンティティ（剛体・群・キャラ）。なければ entt::null。
    // キャラ（CharacterVirtual）にはボディが無く bodyId では引けないので、結果に直接持たせる。
    entt::entity       entity   = entt::null;
};

// 物理の診断値（MCP get_physics_state / perf_stats・診断・テスト用）。
struct PhysicsStats
{
    bool     initialized = false;
    // Jolt の上限（Initialize で固定）と現在の使用量
    uint32_t maxBodies = 0, maxBodyPairs = 0, maxContactConstraints = 0;
    uint32_t bodies = 0, activeBodies = 0;
    uint32_t tempAllocatorBytes = 0;       // 固定の一時メモリ領域のサイズ（超えた分は malloc に逃がす）
    // PhysicsSystem::Update の戻り値（EPhysicsUpdateError）の累積。0 = 一度も上限を超えていない。
    // bit0 ManifoldCacheFull / bit1 BodyPairCacheFull / bit2 ContactConstraintsFull
    uint32_t updateErrorFlags = 0;
    uint64_t updateErrorSteps = 0;          // 上限超過があったステップ数（セッション内）
    uint32_t failedBodies = 0;              // 上限などで Jolt のボディを作れなかった数（セッション内）
    uint64_t droppedSteps = 0;              // フレームのステップ数の上限（kMaxStepsPerFrame）で捨てたステップ数
    uint32_t lastFrameSteps = 0;            // 直近のフレームで進めた固定ステップ数
};

class PhysicsSystem
{
public:
    PhysicsSystem();
    ~PhysicsSystem();

    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    void Initialize();
    void Update(f32 dt, entt::registry& registry);
    void Shutdown();

    // 地形コライダーの追従。高さ配列を編集した側が Terrain::_colliderDirty を立てておくと、
    // 次の Update（と手動呼び出し）で Jolt の HeightFieldShape を作り直す。
    // ＝Play 中に地形を彫っても当たり判定がズレない。
    void RefreshTerrainColliders(entt::registry& registry);

    // スカルプトメッシュ（彫った異形）のコライダー追従。頂点を編集した側が
    // SculptMesh::_colliderDirty を立てておくと、次の Update（と手動呼び出し）で
    // Jolt の MeshShape を作り直す。MeshShape の構築は重いので、エディタ側は
    // ストローク終了時にだけ _colliderDirty を立てること（ドラッグ中に毎フレームやると詰まる）。
    void RefreshSculptColliders(entt::registry& registry);

    // インスタンス群（InstanceGroup + 静的 RigidBody + MeshCollider）のコライダー。インスタンス ≤128 個ごとに
    // StaticCompoundShape の静的ボディ 1 つ（形状はモデル単位の meshShapeCache を共有し、拡縮だけ ScaledShape）。
    // 群の InstanceSet が差し替わった / 群や祖先の Transform が動いた（Play 中）ときに作り直す。
    void RefreshInstanceGroupColliders(entt::registry& registry);
    // 群が張っているボディの数（診断・テスト用）。群でなければ 0。
    size_t InstanceGroupBodyCount(entt::entity e) const;

    // Entity の物理体を登録/解除
    // Play 中に足された RigidBody / CharacterController を拾って登録する（Update の頭で毎フレーム）。
    // これが無いと、実行中に生成したエンティティの物理が無言で効かない。
    void RegisterPendingPhysicsBodies(entt::registry& registry);
    // 逆に、消えたエンティティのボディを Jolt から外す（Scene::Remove は registry.destroy
    // を呼ぶだけなので、これが無いと当たり判定だけが残り、body 上限も食い潰す）。
    void ReleaseOrphanedPhysicsBodies(entt::registry& registry);

    void RegisterBody(entt::registry& registry, entt::entity entity);
    void UnregisterBody(entt::registry& registry, entt::entity entity);
    void UnregisterAllBodies(entt::registry& registry);

    // CharacterController（CharacterVirtual）の生成/破棄。RegisterBody と同じライフサイクル
    // （Play 開始/シーンロードで生成、Stop/Shutdown で破棄）。
    void RegisterCharacter(entt::registry& registry, entt::entity entity);
    void UnregisterCharacter(entt::registry& registry, entt::entity entity);
    void UnregisterAllCharacters(entt::registry& registry);

    // CharacterVirtual を 1 ステップ進める（固定 dt ループ内から呼ぶ）。
    // 入力（_desiredVel/_jumpQueued）→ 速度合成 → ExtendedUpdate → _grounded/_verticalVel 更新。
    void StepCharacters(f32 fixedDt, entt::registry& registry);

    // マルチプレイのクライアント予測リコンシリエーション専用(フェーズ⑦b)。
    // 指定した1体だけ StepCharacters と同じロジックで ExtendedUpdate を進める
    // (他のキャラ/剛体には一切触れない)。該当キャラが無ければ何もしない。
    void StepSingleCharacter(entt::entity entity, f32 fixedDt, entt::registry& registry);

    // 予測リコンシリエーションの補正用: CharacterVirtual の位置を直接テレポートする
    // (ExtendedUpdate を経由しない、SetPositionAndRotation の Character 版)。
    void SetCharacterPosition(entt::entity entity, DirectX::XMFLOAT3 pos);

    // CharacterVirtual の位置を Transform に書き戻す（accumulator ループ後に 1 回）。
    void SyncCharactersToTransforms(entt::registry& registry);

    // 物理操作 API
    void ApplyForce(uint32_t bodyId, DirectX::XMFLOAT3 force);
    void ApplyImpulse(uint32_t bodyId, DirectX::XMFLOAT3 impulse);
    void SetLinearVelocity(uint32_t bodyId, DirectX::XMFLOAT3 vel);
    DirectX::XMFLOAT3 GetLinearVelocity(uint32_t bodyId) const;
    void SetPosition(uint32_t bodyId, DirectX::XMFLOAT3 pos);

    // レイキャスト。normal は Body::GetWorldSpaceSurfaceNormal（BodyLockRead 越し）で
    // 取る**本物の面法線**（HeightField / Mesh でも三角形の法線が返る）。
    // ignoreBody に自分のボディ ID を渡すとセルフヒットを除外する（0xFFFFFFFF で無効）。
    //
    // ★かつてここには normal を (0,1,0) で固定して返す Raycast と、本物を返す RaycastEx の
    //   2 本があった。フェイク側は「Lua の physics:raycast が依存しているかもしれない」
    //   という理由で据え置かれていたが、実際には依存しているコードが 1 行も無かったので
    //   削除して 1 本に統一した（2026-07-30）。normal というフィールドがあるのに嘘を返す
    //   のは、呼んだ側から嘘だと分からない一番たちの悪い形なので、二度と分岐させないこと。
    // ★キャラ（CharacterVirtual）はボディを持たないが、レイ / オーバーラップには「カプセル」として出る
    //   （当たったら RaycastHit::entity がキャラのエンティティ）。ただしレイの始点がキャラのカプセルの中にある
    //   ときは、そのキャラを無視する（カメラ・銃口が自分のカプセルの中にある使い方で自分に当たらない）。
    // ignoreEntity: そのエンティティの剛体 / 群 / キャラを除外する（Lua の physics:raycast の 4 つ目の引数）。
    // includeCharacters=false: 従来どおりキャラを無視する（足の IK・音の遮蔽・AI の視線など、キャラを地面 / 遮蔽物として
    //   扱いたくない内部用途。キャラを数えるのは Lua / MCP の raycast のみ）。
    RaycastHit Raycast(DirectX::XMFLOAT3 origin,
                       DirectX::XMFLOAT3 direction,
                       f32 maxDistance = 1000.0f,
                       uint32_t ignoreBody = 0xFFFFFFFF,
                       entt::entity ignoreEntity = entt::null,
                       bool includeCharacters = true) const;

    // --- 空間クエリ（Broadphase）---
    // out[0..cap) に entity を書き込み、実際に書いた数を返す。cap 超えは切り捨て。
    // physics 未初期化 / out が null / cap==0 なら 0 を返す。
    // ★判定は【形状】（球 / 箱 vs 相手のコライダーの実形状。回した壁・メッシュの凹みも見る）。
    //   以前はボディの AABB だけで、回した薄い壁の AABB の四隅や、メッシュの AABB 内のどこでも当たっていた。
    //   キャラ（CharacterVirtual）もカプセルとして含む。ignoreEntity は結果から除く。同じエンティティは 1 回だけ返す。
    size_t OverlapBox(const DirectX::XMFLOAT3& center,
                      const DirectX::XMFLOAT3& halfExtents,
                      entt::entity* out, size_t cap,
                      entt::entity ignoreEntity = entt::null) const;

    size_t OverlapSphere(const DirectX::XMFLOAT3& center,
                         float radius,
                         entt::entity* out, size_t cap,
                         entt::entity ignoreEntity = entt::null) const;

    // bodyId → entity 逆引き（Raycast の結果を entity に紐付けるため。MCP/Lua 両方から使う）。
    // 見つからなければ entt::null。
    entt::entity EntityForBody(uint32_t bodyId) const;

    // --- 接触コールバック（EventBus 経由）---
    // EventBus を設定する。Initialize より前でも後でも可。
    // Null を渡すと以後のコールバック発火を停止する（PhysicsSystem は所有しない）。
    void SetEventBus(EventBus* bus) { m_eventBus = bus; }

    // --- 停止/ステップ（時間モデル用、Phase 4 の他機能と共用）---
    void SetPaused(bool paused) { m_paused = paused; }
    void Step(float dt);                  // 1固定ステップを即時実行（Jolt だけ。Transform へは書き戻さない旧形）
    // 手動ステップ（physics:step）: Jolt を 1 ステップ進め、キネマティックへの送り・キャラの 1 ステップ・
    // Transform への書き戻し（補間なし）まで行う。pause 中の駒送りで見た目が動くのはこちら。
    void Step(float dt, entt::registry& registry);
    void SetGravity(DirectX::XMFLOAT3 g);

    bool IsInitialized() const { return m_initialized; }
    void ResetAccumulator() { m_accumulator = 0.0f; }

    // 診断値。上限超過（Update の戻り値）・作れなかったボディ・捨てたステップを数える。
    PhysicsStats GetStats() const;

private:
    void SyncTransformsToPhysics(entt::registry& registry, f32 dt);
    void SyncPhysicsToTransforms(entt::registry& registry);
    // 動的剛体の「1 固定ステップ前」の姿勢を控える（描画補間の起点）。物理ステップの直前に呼ぶ。
    void CapturePrevBodyPoses();
    // キャラ 1 体ぶんの 1 固定ステップ（StepCharacters と StepSingleCharacter の共通本体）。
    // recordPrev=true なら描画補間用に踏む前の位置を控える。
    void StepOneCharacter(entt::entity entity, f32 fixedDt, entt::registry& registry, bool recordPrev);
    // ContactListener の pending をメインスレッドで EventBus へ流す。
    void FlushPendingContacts();
    // 補間係数 alphaOverride >= 0 ならその値を使う（手動ステップ後は 1 = 補間なし）。
    void SyncPhysicsToTransformsAlpha(entt::registry& registry, f32 alphaOverride);
    void SyncCharactersToTransformsAlpha(entt::registry& registry, f32 alphaOverride);
    // 1 回の Jolt Update と戻り値（上限超過）の記録。
    void StepJolt(f32 dt);
    // 静的 / キネマティックの Transform・スケール・コライダー部品の変更を Jolt へ反映する（Play 中）。
    void FollowStaticChanges(entt::registry& registry);
    // 登録に失敗したことの記録（上限超過の要約通知 / 再試行の間引き）。
    void NoteBodyCreateFailed();
    void LogSessionSummary();

    struct JoltImpl;
    std::unique_ptr<JoltImpl> m_impl;

    // bodyId → entt::entity の逆引き（RegisterBody 時に追加、Unregister 時に削除）。
    std::unordered_map<uint32_t, entt::entity> m_bodyToEntity;

    // インスタンス群のボディ（群のエンティティごと）。RigidBody::bodyId には先頭のボディを入れる（「登録済み」の印）。
    struct GroupBodies
    {
        std::vector<uint32_t> bodyIds;
        uint64_t setId    = 0;    // 作った時の InstanceSet の id
        uint64_t worldKey = 0;    // 作った時のワールド行列列のハッシュ（InstanceSet::worldKey）
        uint32_t skipped  = 0;    // 形状を作れずに飛ばしたインスタンス数
    };
    std::unordered_map<entt::entity, GroupBodies> m_groupBodies;
    void RegisterGroupBodies(entt::registry& registry, entt::entity entity);
    void UnregisterGroupBodies(entt::registry& registry, entt::entity entity, bool removeFromJolt);

    EventBus* m_eventBus = nullptr;   // 外部所有。PhysicsSystem は解放しない。
    bool      m_paused   = false;

    bool  m_initialized = false;
    f32   m_accumulator = 0.0f;

    // ---- 診断（PhysicsStats の元）----
    uint32_t m_updateErrorFlags = 0;
    uint64_t m_updateErrorSteps = 0;
    uint32_t m_updateErrorLogged = 0;      // すでに ERROR を出した EPhysicsUpdateError のビット（セッションごとに 1 回）
    uint32_t m_failedBodies = 0;
    uint32_t m_failedBodiesLogged = 0;
    uint64_t m_droppedSteps = 0;
    uint32_t m_lastFrameSteps = 0;
    uint64_t m_frame = 0;                  // Update の通し番号（再試行の間引き用）
    // 失敗した登録の再試行を間引く（メッシュが空など。1 エンティティが毎フレーム警告を出し続けない）。
    std::unordered_map<entt::entity, uint64_t> m_regBackoff;   // entity → 次に再試行してよいフレーム
    std::unordered_map<entt::entity, bool>     m_regWarned;    // 警告済みのエンティティ

    // 時間の扱い。1 フレームに進める固定ステップ数の上限（遅い PC・ヒッチで物理が追いつけず
    // 余計に重くなる「死のスパイラル」を避ける。超えた分の時間は捨てる）。
    // 0.25s（15fps 未満のフレーム）を超える dt は 0.25s に丸めてからステップ数を数える。
    static constexpr f32 kMaxFrameDt       = 0.25f;
    static constexpr int kMaxStepsPerFrame = 8;

    static constexpr f32 kFixedTimeStep  = 1.0f / 60.0f;
    static constexpr int kCollisionSteps = 1;
};

} // namespace dx12e
