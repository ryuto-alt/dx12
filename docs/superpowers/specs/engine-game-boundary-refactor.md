# エンジン/ゲーム境界 再設計 — 封印ランタイム化 設計書

- ステータス: ドラフト（リードアーキテクト統合版）
- 日付: 2026-06-22
- スコープ: エンジンを「ゲームを1つも知らない封印されたランタイム」へ作り替え、ユーザーが `src/` を一切触らず新ジャンル（2D/2.5D・3Dアクション・非リアルタイム・RTS/大量エンティティ）を作れるようにする。
- 進め方: big-bang（境界を一気に再設計）。ただし既存サンプル（title→level1→clear）は各フェーズ末で動作維持しつつ新アーキへ移植する。
- 部品モデル: ハイブリッド（コア部品=C++高速安定 / ゲームロジック部品=Lua・データ駆動で再コンパイル不要）。

---

## 0. このドキュメントの読み方（最重要の結論）

各領域レビューが一致して突きつけた事実をまず明記する。**「部品の置き場所をデータ化する」だけでは封印ランタイムにならない。** 新ジャンルが本当に要求するのは多くの場合「新しいゲームロジック部品」ではなく「新しい描画/シミュレーション/入力の能力（capability）」であり、それは Lua の `component{}` 宣言では一切追加できない。

したがって本設計は2本立てで構成する。

1. **能力カタログ（Capability Catalog）の確定**: 4ジャンルを成立させるために C++ コアが先に実装して公開する「ジャンル中立な汎用能力」の閉じた集合。これを v1 で実装する。**ここに無い能力は原理上 C++/HLSL 改造が要る** ことを正直に開示する（§7）。
2. **ハイブリッド部品系 + データ駆動境界**: 能力カタログの上で、ゲームロジックを Lua/データだけで組み立てる仕組み。

「データ駆動化（部品レジストリ・脱enum・prelude外出し）」と「能力拡張（ortho カメラ・ワールド2D・空間クエリ・時間モデル・入力拡張）」は別物であり、**両方やらないと2D/非リアルタイム/RTSは作れない**。3D アクションは既存能力でほぼ成立するが、見た目バリエーション（カスタムシェーダ/トゥーン/新パーティクル/新ポスト）はスコープ外で改造が残る（§7.5 で明示）。

---

## 1. 現状の問題サマリ（焼き込み概念 → 行き先）

### 1.1 ジャンル語彙の焼き込み（enum/struct/switch）

| 焼き込み概念 | 現在地 | 新しい行き先 |
|---|---|---|
| `GimmickKind{StaticWall,SpikePulse,SlideX,SlideZ}` | `Components.h:154-160` | **削除**。`assets/components/Mover.lua` / `Hazard.lua`（動き=汎用Transformアニメ、deadly=接触イベント+Luaロジック） |
| `Gimmick` struct（period/phase/amplitude/threshold/deadly/solid） | `Components.h:162-171` | **削除**。Lua部品の `properties` へ |
| `gimmicks()` バインド | `ScriptEngine.cpp:324-346` | **撤去**。`scene:query("hazard")`（タグ列挙）へ |
| `TriggerActionType`（11値 enum）+ `UpdateTriggers` switch | `Components.h:310-322` / `ScriptEngine.cpp:1452-1594` | **撤去**。Trigger は「領域 overlap → `events:emit(eventName)`」へ縮退。11アクションは `assets/lib/trigger_actions.lua` の Lua ハンドラ表へ |
| `TriggerShape/TriggerWhen` + `filter='Player'` 既定 | `Components.h:306-307,341` / `ScriptEngine.cpp:1547` | shape/when は Trigger コア部品のパラメータに残す。filter は **Tag集合**へ一般化、`Player` 既定を撤廃 |
| `ParticleEmitter.kind/blend`（int 0..7 固定） | `Components.h:271-301` | kind を **不透明シェーダID（文字列）** + パラメータ表へ。blend は常時明示パラメータ化。プリセットは `assets/data/fx/*.json` |
| `GridPlane` 部品 | `Components.h:87-90` | マテリアル/シェーダフラグ or L3 データへ |
| `SceneFlow` の `start/next/onFail` | `SceneFlow.cpp:29-43` | 中立だが `onFail`（=失敗進行語彙）は L3 Lua フローへ。遷移グラフ自体は温存 |

### 1.2 ジャンルヘルパの焼き込み（kPrelude C++文字列）

| 焼き込み概念 | 現在地 | 新しい行き先 |
|---|---|---|
| `moveTopDown` / `Actor` / `_blocked` / `reached` | `ScriptEngine.cpp:797-875` | `assets/lib/topdown.lua` / `actor.lua`（当たりは新 `physics:overlapBox` 委譲） |
| `cameraFollow` / `cameraTPS` / `cameraLockOn` | `ScriptEngine.cpp:879-974` | `assets/lib/camera.lua` |
| `win` / `goToScene` | `ScriptEngine.cpp:889-890` | `assets/lib/flow.lua`。エンジンは中立 `game.loadScene/nextScene/transition` のみ |
| `FX.{explosion,shockwave,...}` / `vfx.*` | `ScriptEngine.cpp:983-1112` | `assets/lib/fx.lua` + `assets/data/fx/*.json`。`fx:burst/ring/beam/pulse` 生APIは **L1温存** |
| `KEYS` 固定マップ / `KEY_*` 定数 | `ScriptEngine.cpp:411-429,772-777` | VK全域は L1 公開。意味付けは `input:bindAction` データ表（`assets/data/input.json`） |
| 固定エンティティ命名規約（Player/Goal/GameCamera/Wall1..） | prelude + docs | **撤廃**。Tag参照 or パラメータ必須化 |
| `events` バス / `ui` / `blackboard` | `ScriptEngine.cpp:781-793` | **L1温存**（ジャンル中立）。ただし events は C++ `EventBus` へ格上げし Lua は薄いバインドに |

### 1.3 ビルド/配布の焼き込み

| 焼き込み概念 | 現在地 | 新しい行き先 |
|---|---|---|
| 単一 exe にエディタ+ランタイム同居、BuildGame は exe 自身をコピー | `CMakeLists.txt:39-57` / `Application.cpp:2305-2414` | `dx12-runtime.exe`（封印・Editor非リンク）と `dx12-editor.exe` に分割。BuildGame は runtime exe をコピー |
| `Project.cpp` の `TemplateSceneJson/TemplateGameLua`（fps/tps/empty を C文字列焼き込み） | `Project.cpp:63-288` | `templates/<id>/`（manifest+scene+lua）をディスクから読む |
| ランチャーの FPS/TPS/空 ハードコードボタン | `ProjectManager.cpp:313-355` | `templates/` 走査で manifest から動的生成 |
| `--validate` の `type==7/8` ハードコード | `main.cpp:117-135` | コンポーネント自己記述スキーマ駆動の検証へ |
| 新規シーンの「床+箱+カメラ」既定配置 | `Application.cpp:2557-2576,2719-2772` | `project.json` の `newSceneTemplate`（データ）へ |
| `target_include_directories(.. PUBLIC src/..)` で src 全公開 | 全 `CMakeLists.txt` | `include/engine/` のみ公開ヘッダに隔離 |

### 1.4 能力の欠如（descriptor では足せない＝v1 でコア実装が必須）

| 欠如能力 | 影響ジャンル | v1 帰属 |
|---|---|---|
| Orthographic 投影 + `projectionType` | 2D/2.5D, RTS俯瞰, ボード | `CameraComponent` フィールド + `Camera.cpp` |
| ワールド空間 `Sprite2D` ECS部品 + SpriteRenderer のカメラ連動/深度ソート | 2D/2.5D, カード | コア部品 + `SpriteRenderer` 改修 |
| Tilemap（or instanced-quad 表現） | 2D/パズル/RTS | データ駆動部品 + instanced-quad 供給経路 |
| `physics:overlapBox/overlapSphere/sphereCast`（broadphase 範囲クエリ） | RTS選択/近接, 軽量当たり, Trigger | `PhysicsSystem` 新設 |
| 物理から独立した空間インデックス（uniform-grid） | RTS（4096 body 超） | `SpatialIndex`（Jolt非依存） |
| 接触/衝突イベント（ContactListener → EventBus） | 3Dアクション, ハザード | `PhysicsSystem` + `EventBus` |
| Tag/Layer 機構 + タグクエリ | filter撤廃, RTS群管理 | `Tag` コア部品 + `scene:query(tag)` |
| 汎用データ部品（string→variant）= Lua定義状態の器 | RTS(HP/陣営), カード(手札), ボード(セル) | `DataComponent` + `ScriptComponents` |
| 時間モデル（realtime/manual/paused, step） | パズル/ボード/ターン制 | メインループの更新ストラテジ + `time:*`/`physics:step` |
| 入力拡張（全マウスボタン/ホイール/絶対座標/アクションマップ/ピッキング） | RTS/カード/ボード, リバインド | `InputSystem` + `ActionMap` + `input:pickEntity` |
| 複数ビューポート/複数カメラ | RTS(ミニマップ)/分割画面/カード | レンダラ改修（**v1 スコープ外、§7 で開示**） |
| データ駆動マテリアル/シェーダID化（パーティクル/ポスト/トランジション） | 3D見た目バリエーション | **v1 スコープ外、§7 で開示** |
| ゲームパッド(XInput) | コンソール風アクション | **v1 スコープ外、§7 で開示** |

---

## 2. ターゲットアーキテクチャ

### 2.1 3層モデルと責務境界

```
┌─────────────────────────────────────────────────────────────┐
│ L3  ゲーム/プロジェクト資産（エンジン外・ここだけ触る）            │
│   assets/components/*.lua   …… データ駆動コンポーネント(Health等) │
│   assets/lib/*.lua          …… moveTopDown/cameraTPS/FX 等ヘルパ  │
│   assets/data/*.json        …… fxプリセット/入力バインド/シーンテンプレ│
│   scenes/*.json  game.json/.dx12proj                            │
├─────────────────────────────────────────────────────────────┤
│ L2  データ駆動コンポーネント機構（C++・封印）                     │
│   ComponentRegistry（typeName→descriptor）                      │
│   ScriptComponents（1エンティティに複数Lua部品）                  │
│   スキーマ駆動 Inspector / 汎用シリアライズ / SnapshotUndo         │
├─────────────────────────────────────────────────────────────┤
│ L1  エンジンプリミティブ＝能力カタログ（C++・封印・無改造）         │
│   コア部品: Transform/MeshRenderer/Camera(persp+ortho)/Light/    │
│             RigidBody/Collider/Skeletal/NodeAnim/Audio/NameTag/  │
│             Tag/Sprite2D/DataComponent                          │
│   能力API: physics:overlap*/onContact/step, scene:query(tag),   │
│            input:bindAction/pickEntity/mouse*, camera ortho,    │
│            time:pause/step, fx:burst/ring/beam/pulse, ui:*,     │
│            events(EventBus), game.* サービス, SpatialIndex       │
└─────────────────────────────────────────────────────────────┘
```

**封印の不変条件**: `EngineRuntime`（= L1+L2 を含む C++ ライブラリ群）のソースに `Gimmick|TriggerActionType|moveTopDown|cameraTPS|win|"Player"` 等のジャンル語彙が **grep 0件**。CI で機械チェックする（§9 自己診断）。

### 2.2 エンジン（封印ランタイム）の責務

**必ずエンジンに残す（重い基盤・Lua化不可）**: D3D12/GPU、Jolt物理ソルバ、レンダパイプライン、メッシュ/テクスチャロード、sol2 バインド土台、シリアライザのリフレクション機構、Inspector 自動生成エンジン、空間インデックス。

**エンジンに入れてはいけない（L3 へ）**: ジャンル語彙（win/lose/goal/Player/Enemy/health/score/trap/deadly）、完成した高レベル挙動（moveTopDown/cameraTPS/FX.explosion）、閉じた意味付け enum（GimmickKind/TriggerActionType/ParticleKind の意味）、固定命名規約・デフォルト配置。

### 2.3 ハイブリッド部品系（L2 の仕組み）

#### 2.3.1 ComponentRegistry（2層：ランタイム面 / エディタ面）

レビュー指摘（`drawInspector` が `EditorContext` を要求すると封印ランタイムがエディタに依存する）を受け、レジストリを**2層に分離**する。

- **RuntimeComponentRegistry**（`EngineRuntime`、エディタ非依存）: `typeName → { serialize, deserialize, applyOverride, postLoad, source(Core|Lua) }`
- **EditorComponentRegistry**（`EditorLib`、`typeName` をキーに後付け登録）: `{ fields[FieldDef], customDraw, icon, tint, addable, category }`

ランタイム exe は EditorComponentRegistry を一切リンクしない。

```cpp
// include/engine/ecs/ComponentRegistry.h  （公開・中立ヘッダ。具象コア型を include しない）
enum class ComponentSource { Core, Lua };

struct RuntimeComponentInfo {
    std::string typeName;
    ComponentSource source;
    std::function<void(const entt::registry&, entt::entity, nlohmann::json&)>   serialize;
    std::function<void(entt::registry&, entt::entity, const nlohmann::json&)>   deserialize;
    std::function<void(entt::registry&, entt::entity, const nlohmann::json&)>   applyOverride; // Play時エディタ値上書き（第一級メンバ）
    std::function<void(entt::registry&, entt::entity)>                          postLoad;      // ConvexHull の Mesh 依存再生成など順序依存
};
class RuntimeComponentRegistry {
public:
    static RuntimeComponentRegistry& Get();
    void Register(RuntimeComponentInfo);
    const RuntimeComponentInfo* Find(const std::string& typeName) const;
    void ForEach(const std::function<void(const RuntimeComponentInfo&)>&) const;
};
```

```cpp
// FieldDef は std::any を避け、閉じた variant に限定（ABI/再コンパイル抑制）
struct FieldDef {
    std::string name, label, tooltip, category;
    enum class Type { Float, Int, Bool, String, Vec3, Color, Entity, Enum } type;
    std::vector<std::string> choices;                     // Enum/combo（kind/blend/motionType をデータ化）
    bool hasRange = false; float minVal = 0, maxVal = 0;
    std::variant<float,int,bool,std::string,DirectX::XMFLOAT3> def;
};
```

#### 2.3.2 コア部品の一元登録（X-macro）

新コア部品追加コストを「`.def` に1行 + struct 定義」に集約。`SerializeEntityJson`/`InstantiateEntityJson`/`ApplyOverrides` の型ごと `if(all_of<T>)` 連鎖（`SceneSerializer.cpp:81-346,468-841,965-1027`）を撤廃。

```cpp
// src/ecs/EngineComponents.def
COMP(Transform, "transform",
     FIELD(position, Vec3) FIELD(rotation, Vec3) FIELD(scale, Vec3))
COMP(CameraComponent, "camera",
     FIELD_ENUM(projection, {"Perspective","Orthographic"})
     FIELD_RANGE(fovDegrees, Float, 1, 179) FIELD_RANGE(orthoSize, Float, 0.1, 500)
     FIELD(nearClip, Float) FIELD(farClip, Float) FIELD(isActive, Bool))
COMP(RigidBody, "rigidBody",
     FIELD_ENUM(motionType, {"Static","Kinematic","Dynamic"})
     FIELD(mass, Float) FIELD(restitution, Float) FIELD(useGravity, Bool))
COMP(Tag, "tag",  FIELD(tags, StringList))
COMP(Sprite2D, "sprite2d",
     FIELD(texturePath, String) FIELD(layer, Int) FIELD(size, Vec2)
     FIELD(uvMin, Vec2) FIELD(uvMax, Vec2) FIELD(color, Color) FIELD(worldSpace, Bool))
// ... PointLight/Dir/Spot/各Collider/Audio/MeshRenderer/Skeletal/NodeAnim/NameTag ...
```

`Material`/`UV Tiling`/`ConvexHullCollider`/primitive マーカーのような特殊ケースは `customDraw`（エディタ面）と `postLoad`（ランタイム面の順序依存）で逃がす。`customDraw` は **コア部品専用** とし、Lua 部品は使えない契約（さもないと Lua が C++ 描画を要求して脱結合が破れる）。

#### 2.3.3 Lua部品の宣言と複数アタッチ

`LuaScript`（単一 env/self）を **`ScriptComponents{ vector<ScriptInstance{ typeName, props, env, self, started, loadError }> }`** に置換。これにより Health + Inventory + Region を同一エンティティに貼れる。これは「最重要前提」ではなく**確定仕様**として固定する（レビュー指摘）。

```lua
-- assets/components/Health.lua  （現 Spinner.lua の properties を component{} に発展）
component {
  name = "Health", category = "Gameplay", icon = "heart",
  fields = {
    { name="max",  type="float", default=100, min=1, max=9999, label="最大HP" },
    { name="cur",  type="float", default=100, label="現在HP" },
    { name="team", type="enum",  default="Player", choices={"Player","Enemy","Neutral"} },
  },
  onStart  = function(self) self.cur = self.max end,
  onEvent  = function(self, name, data) if name=="damage" and data.target==self.entityId then self.cur = self.cur - data.amount end end,
  onUpdate = function(self, dt) if self.cur <= 0 then events:emit("died", {who=self.entityId}) end end,
}
```

#### 2.3.4 ライフサイクルの一元管理

5つのライフサイクルメソッド（Attach/OnPlayStart/OnPlayStop/Update/Reload）+ `InitializeLuaScriptInstance` を、`ScriptInstance` のコレクション反復として**1つのマネージャに集約**し、`EnterPlayMode/EnterEditorMode/DoRuntimeSceneLoad` の3重複を解消する。

**ホットリロード時のフィールドマージ規則（明文化）**:
- props 由来フィールド: `name` 一致 かつ `type` 一致のみ保持。type 変更は default 再適用。enum 候補外の値は default フォールバック。entity 参照は再解決し失敗で nil。
- 非props実行時状態（`self._t`, `self._was` 等）: 既定では破棄。保持したい部品は `onReload(self, old)` フックで明示移行。
- `onStart` 再実行ルール: リロード時は再実行しない（`started` を保持）。`RebuildScene`（game.lua 全体再構築）時のみ再実行。

監視対象は game.lua のみ（現状）から `assets/{lib,components}/**.lua` に拡張する。

### 2.4 イベント/Trigger 汎用化

`events` バスを **C++ `EventBus`**（Lua 非依存、`entt::entity`+string+callback）へ格上げし、Lua の `events` テーブルはその薄いバインドにする。C++ サブシステム（衝突/トリガ/シーンライフサイクル/ターン）が同じバスに流す。

```cpp
// include/engine/scripting/EventBus.h
struct EngineEvent {
    std::string name;                  // "engine.trigger.enter" / ゲーム独自名
    entt::entity source = entt::null, other = entt::null;
    // ヒープ確保を避けるため小さな固定ペイロード（map ではなく SoA / 小vector）
    struct KV { std::string key; std::variant<double,std::string,bool> val; };
    std::vector<KV> data;
};
class EventBus {
public:
    using Handler = std::function<void(const EngineEvent&)>;
    uint32_t On(const std::string& name, Handler h);
    uint32_t On(const std::string& name, entt::entity scope, Handler h); // entityスコープ：Destroyで自動Off
    void Off(uint32_t id);
    void OnEntityDestroyed(entt::entity e);   // entt on_destroy シグナルから呼ぶ
    void Emit(const EngineEvent&);            // 即時（既存 events:emit 互換）
    void Post(EngineEvent);                   // フレーム末遅延キュー（列挙中破壊を回避）
    void Flush();
    void Clear();                             // OnPlayStart で全消去
};
```

**Trigger の縮退**: `Trigger` struct は領域判定 + 発火イベント名のみへ。`UpdateTriggers` は overlap 判定だけ行い `enterEvent/exitEvent/stayEvent` を `Post` する。subsystem 直叩きは全廃。`filter` は `filterTags`（タグ集合）へ。**判定ループは C++ に残す**（汎用 overlap であってジャンル語彙ではない）。Region 部品はパラメータ宣言データに留め、Lua から毎フレーム `overlapBox` を呼ばせない（性能退行回避＝レビュー指摘）。

```cpp
struct Trigger {
    int shape = 0;                       // 0=Box,1=Sphere
    XMFLOAT3 halfExtents{1,1,1}; float radius = 1; XMFLOAT3 offset{0,0,0};
    std::vector<std::string> filterTags; // 空=全対象（"Player"既定撤廃）
    bool once = false;
    std::string enterEvent, exitEvent, stayEvent;
    std::unordered_set<entt::entity> _inside; bool _firedOnce = false; // ランタイム専有
};
```

旧11アクションは `assets/lib/trigger_actions.lua` の `events:on` ハンドラ表として移植。新アクション = Lua ハンドラ追加（再コンパイル不要）。

**ノーコード体験の維持**（レビュー指摘）: 上級者は Lua ハンドラ。非プログラマ向けには標準同梱の `assets/_engine_std/components/ActionTable.lua`（「イベント名 + loadScene/setProp/playSound 等の中立アクションを列挙」）部品を提供し、エディタはその `fields/enum` を descriptor 経由で編集する二段構え。

### 2.5 ゲーム制御サービスの整理（IGameServices）

6+本の個別コールバック setter を単一の `IGameServices` 抽象に集約し、`ScriptEngine` はこれ1本を受ける。Lua からは `game.*` 名前空間に統一（`game.loadScene/nextScene/quit/transition/save/load`）。旧名（`loadScene/nextScene/quit/fadeToScene/saveNum/loadNum`）は後方互換エイリアスで温存（サンプル無改造）。

### 2.6 時間モデル（メインループから引き剥がす）

`Application::Update()` の `m_engineMode==Playing` 直書きゲート（`Application.cpp:1242,1289-1322`）を、`ProjectRuntimeConfig.timeModel` 駆動の**更新ストラテジ**へ置換。Scene/Physics/Particle/Audio の dt 供給を **1箇所に集約**し、`timeModel`（realtime / manual / paused）で分岐。Lua から `time:pause()/time:setScale(s)`、`physics:step(dt)/physics:setPaused(b)`、`frame:requestStep()` を呼べる。ターン制は「入力待ちで dt=0、確定したら1ステップ」が成立。

### 2.7 camera/physics null 安全化

ボード/カードで physics/camera 無し起動を成立させるため、`ScriptEngine.Initialize` 署名の null 許容だけでなく **Application 側の全参照点**（カメラ同期 `1228-1238,1987-2004`、Audio リスナー供給 `1300`、Play/Stop ライフサイクルのカメラチェック `2066`、物理 Shutdown/Init/再登録 3箇所）を null 安全化する。

### 2.8 CMake ターゲット依存図

```
                         ┌──────────────┐
                         │   spdlog     │
                         └──────┬───────┘
                                │
              ┌─────────────────▼──────────────────┐
              │  Core (PathResolver/PublicHeaders)  │
              └──┬────────┬────────┬────────┬───────┘
        ┌────────▼──┐  ┌──▼────┐ ┌─▼─────┐ ┌▼──────┐
        │ Graphics  │  │ Input │ │ Audio │ │Project│
        └────┬──────┘  └───┬───┘ └──┬────┘ │ Data  │
   ┌─────────▼────┐        │        │      └───┬───┘
   │ Animation    │        │        │          │
   └──┬───────────┘        │        │          │
   ┌──▼──┐ ┌──────────┐    │        │          │
   │ ECS │ │ Resource │    │        │          │
   └──┬──┘ └────┬─────┘    │        │          │
      │    ┌────▼─────┐    │        │          │
      │    │ Renderer │    │        │          │
      │    └────┬─────┘    │        │          │
   ┌──▼─────────▼──────┐   │        │          │
   │      Scene        │   │        │          │
   │ (+ComponentReg    │   │        │          │
   │  +EventBus base)  │   │        │          │
   └──┬────────────┬───┘   │        │          │
   ┌──▼─────┐  ┌───▼───────▼────────▼──┐       │
   │Physics │  │      Scripting        │       │
   │(+overlap│ │ (sol2/Lua, IGameSvc)  │       │
   │+contact)│ └───────────┬───────────┘       │
   └────┬────┘             │                   │
        │   ╔══════════════▼═══════════════════▼═══════╗
        └──►║ EngineRuntime (INTERFACE: 上記 L1+L2 集合) ║
            ╚════════════╤═════════════════╤════════════╝
                         │                 │
              ┌──────────▼────┐   ┌─────────▼──────────┐
              │ dx12-runtime  │   │ EditorLib          │
              │ (封印exe)      │   │ +ProjectLauncher   │
              └───────────────┘   └─────────┬──────────┘
                                  ┌──────────▼───────┐
                                  │  dx12-editor exe │
                                  └──────────────────┘
```

- `EngineRuntime`（INTERFACE）= Core/Graphics/Renderer/Resource/Animation/ECS/Input/Audio/Scene/Scripting/Physics/ProjectData。**ジャンル概念ゼロ**。
- `dx12-runtime.exe` = `EngineRuntime` のみ。`src/runtime_main.cpp` が game.json/.dx12proj を読み「エディタ無し構成」で `Application` を起動。
- `dx12-editor.exe` = `EngineRuntime` + `EditorLib` + `ProjectLauncher`。
- 公開ヘッダは `include/engine/` のみ。各ターゲットの `target_include_directories` から `src/..` 全公開を撤廃。

---

## 3. 不足プリミティブ（v1 能力カタログの API スケッチ）

```cpp
// ---- Camera（2D/見下ろし/RTS/ボード）----
enum class Projection : uint8_t { Perspective, Orthographic };
struct CameraComponent { Projection projection=Projection::Perspective;
    float fovDegrees=60, orthoSize=10, nearClip=0.1f, farClip=1000; bool isActive=false; };
// Camera.cpp: GetProjectionMatrix() を projection で分岐（XMMatrixOrthographicLH 追加）

// ---- 2D 描画供給（ECS→SpriteRenderer）----
struct Sprite2D { std::string texturePath; int layer=0;
    XMFLOAT2 size{1,1}, uvMin{0,0}, uvMax{1,1}; XMFLOAT4 color{1,1,1,1}; bool worldSpace=true; };
// SpriteRenderer: 既存 HUD(ui:image) 経路に加え、Sprite2D を「カメラ連動・ワールド座標・layer/depthソート」で描く供給経路を追加。
//   タイルマップは「Sprite2D の instanced-quad バッチ + データ(アトラス+グリッドJSON)」で表現し、エンジンに『タイル』語彙を入れない。

// ---- 空間クエリ（RTS選択/近接/軽量当たり）----
// physics 有り = Jolt CollideShape/BroadPhaseQuery。out-param でアロケーション回避。
size_t PhysicsSystem::OverlapBox(const XMFLOAT3& c, const XMFLOAT3& half, entt::entity* out, size_t cap);
size_t PhysicsSystem::OverlapSphere(const XMFLOAT3& c, float r, entt::entity* out, size_t cap);
void   PhysicsSystem::SetContactCallback(std::function<void(entt::entity,entt::entity,XMFLOAT3)>);
void   PhysicsSystem::Step(float dt); void PhysicsSystem::SetPaused(bool);
void   PhysicsSystem::SetGravity(XMFLOAT3); // project.runtime 駆動
// physics 非依存（Jolt 4096 body 上限を超える群・2D）：
//   SpatialIndex（uniform-grid, Tag+Transform 索引）。scene:spatialQuery が裏で Jolt or grid を選ぶ統一ファサード。
size_t Scene::SpatialQuery(const Bounds& region, entt::entity* out, size_t cap);

// ---- Tag/クエリ（filter='Player' 撤廃）----
struct Tag { std::vector<std::string> tags; };
// Lua: e:hasTag(t) / e:addTag(t) / scene:query("solid") -> Entity配列（C++で entt view 一括）

// ---- 入力（RTS/カード/ボード/リバインド）----
// InputSystem: 全マウスボタン(L/M/R)/ホイール/絶対座標 を追加。FPSキャプチャをオプトイン。
// ActionMap（C++, data/input.json 唯一の真実源）: input:bindAction("move",{KEY_W=Vec3(0,0,1),...}); input:action("move")->Vec3
// ピッキング: input:pickEntity(screenX,screenY) -> Entity（2D Sprite/UI と 3D mesh 両対応、カメラ投影を使用）

// ---- 時間モデル ----
// engine:setTickMode("realtime"|"manual"); engine:step(n); time:pause()/resume()/setScale(s)

// ---- アニメ完了通知（演出待ち turn scheduler）----
// Animator/NodeAnimator に onFinish/イベントマーカを追加し EventBus へ流す（engine.anim.finished）

// ---- EngineServices（依存緩和）----
struct EngineServices { Scene* scene=nullptr; InputSystem* input=nullptr;
    Camera* camera=nullptr; AudioSystem* audio=nullptr; PhysicsSystem* physics=nullptr;
    ParticleSystem* particles=nullptr; EventBus* bus=nullptr; IGameServices* game=nullptr;
    std::string assetsDir, libDir; };
void ScriptEngine::Initialize(const EngineServices&, const ProjectRuntimeConfig&);
```

---

## 4. 4ジャンル成立証明（エンジン非改造で組める＝v1 能力カタログの組合せ）

各ジャンルを「使用するコア部品 / L1 能力 / L3 で書く Lua部品」に分解し、**追加の C++ 改造が不要**であることを示す。残る穴は §7 に隔離する。

### 4.1 2D/2.5D（見下ろし・横スク）

| 必要 | 充足 |
|---|---|
| 正射投影 | `CameraComponent.projection=Orthographic`（L1コア部品） |
| ワールド2D描画 | `Sprite2D` コア部品 + SpriteRenderer カメラ連動/depthソート（L1） |
| マップ | Sprite2D instanced-quad + `assets/data/tilemap.json`（L3データ） |
| 移動/当たり | `assets/components/TopDownController.lua` + `physics:overlapBox`（軽量AABB） |
| 当たり領域/ゴール | `assets/components/Region.lua`（Trigger縮退→events） |
| 入力 | `input:bindAction("move",...)`（data/input.json） |
| 物理 | 任意（軽量 overlap で足り、Jolt 不要） |

**証明**: 描画は Sprite2D+ortho、ロジックは Lua 部品、当たりは overlap、入力はアクションマップ。すべて v1 能力カタログ内。横スクの重力方向/2D拘束は overlap ベースの自前処理（Jolt不使用）で吸収。→ **非改造で成立**。残る穴: 視差/アイソメtrueソート/ヘックス/ピクセルパーフェクト/オートタイルは §7.1。

### 4.2 3D アクション/アドベンチャー

| 必要 | 充足 |
|---|---|
| 透視カメラ/メッシュ/スケルタル | 既存コア部品（無改造） |
| 物理/レイキャスト | 既存 RigidBody/Collider + 既存 `physics:raycast` |
| 接触イベント | 新 `physics:onContact` → EventBus（L1） |
| カメラ追従 | `assets/lib/camera.lua`（旧 cameraFollow/TPS/LockOn 移植） |
| 移動 | `assets/components/ThirdPersonController.lua` |
| トリガ/扉/罠 | `Region.lua` + `trigger_actions.lua` |
| 演出 | `fx:burst/ring/beam/pulse`（L1）+ `assets/lib/fx.lua` + `data/fx/*.json` |

**証明**: 既存サンプル（title/level1/clear）はこの構成へ移植。接触イベント以外は既存能力。→ **非改造で成立**。残る穴: カスタムシェーダ/トゥーン/新パーティクル見た目/新ポスト/ステートマシンアニメは §7.5。分割画面/ミニマップは §7.4。

### 4.3 非リアルタイム（パズル/ボード/カード）

| 必要 | 充足 |
|---|---|
| 2D/固定カメラ描画 | ortho + Sprite2D（4.1 と同じ L1） |
| 時間停止/手動進行 | `time:pause()` / `engine:step()` / `physics:setPaused()`（L1 時間モデル） |
| 盤面/手札の状態 | `DataComponent`（string→variant）+ Lua部品（L2） |
| マウス操作/ピック | 全マウスボタン+絶対座標+`input:pickEntity`（L1 入力拡張） |
| 演出待ち | `engine.anim.finished` イベント（L1 アニメ通知） |
| ターン進行 | ゲーム側 Lua コルーチン + EventBus（L3） |
| physics/camera 無し起動 | EngineServices null 許容 + Application null 安全化（§2.7） |

**証明**: 時間モデル・入力拡張・ピッキング・アニメ通知・DataComponent が v1 で揃うため、ターンスケジューラは L3 Lua で完結。→ **非改造で成立**。

### 4.4 RTS/大量エンティティ

| 必要 | 充足 |
|---|---|
| 俯瞰/ortho カメラ | `CameraComponent`（L1） |
| 矩形選択/範囲列挙 | `scene:spatialQuery(box)`（Jolt非依存 grid、body上限と無関係） |
| 近接検索 | `physics:overlapSphere`（out-param、アロケーション回避） |
| 地面クリック | `input:pickEntity` + `physics:raycast` |
| ユニット状態（HP/陣営/所属） | `DataComponent` + Lua部品 |
| 群の集合 | `Tag` + `scene:query("team:red")` |
| 大量更新の性能 | **`onUpdateBatch` + POD ミラー部品 + C++ 汎用 Mover/SpatialQuery system**（§6） |
| body 上限 | `project.runtime.maxBodies` データ化。非物理群は body 非登録 + grid |
| audio リスナー | カメラから脱結合（§2.7） |

**証明**: 数千ユニットのホットパスは「Lua はデータ宣言 + C++ 汎用 system が entt view で一括駆動」へ寄せる。エンジンに `SpatialIndex` と `Mover/SpatialQuery` 汎用 system を v1 で実装するため、ゲーム側は C++ を書かない。→ **非改造で成立**。ただし性能は §6 の方針順守が前提。ミニマップ/分割画面は §7.4。

---

## 5. 段階移行計画（big-bang・順序付きフェーズ）

各フェーズ末で **testengine の title/level1/clear が動く** ことを完了条件に含める。フェーズ0〜2が「触らなくても作れる」土台、3〜5が能力カタログ、6が封印確定。

### フェーズ0 — 安全網と基盤ヘッダ分離（最初の一歩・特に具体的）

> **✅ 完了 (2026-06-22) — ビルド/テスト グリーン（挙動不変・追加のみ）**
> - `tests/serialize_roundtrip_test.cpp`（新規）: データ系コア部品 14 種（Transform/Point・Directional・Spot ライト/Camera/Gimmick/AudioSource/ParticleEmitter/Trigger+actions/RigidBody/Box・Sphere・Capsule Collider/LuaScript+5 プロパティ型）の保存フィールド全往復を検証。GPU 非依存（device 依存の mesh/primitive/gridPlane/convexHull/material は対象外）。`SerializeEntity → InstantiateEntity` を経由。
> - `include/engine/ecs/ComponentRegistry.h`（新規）: 中立ヘッダ（具象型 include 無し）。`RuntimeComponentInfo`/`RuntimeComponentRegistry` 宣言。**まだ未配線**（Phase 1 で実装）。
> - `tests/CMakeLists.txt`: `SerializeRoundtripTests` ターゲット追加（`if(TARGET Scene)` ガードで依存ゼロ CI を非破壊）。`include/` を公開ヘッダ root として追加。
> - 検証: VS2022/Ninja の `build/debug` 増分ビルド＋`ctest` で `IndexAllocatorTests` / `SerializeRoundtripTests` ともに Passed、エンジン全ターゲット ビルド成功。
> - **resequence**: 当初 Phase 0 案にあった `EngineComponents.def` / `ComponentReflect.h`（マクロ版）は、リフレクション実装を **`entt::meta` に確定**（§10.0）したため Phase 1 へ移し、entt::meta 登録として実装する。Phase 0 はまず安全網確立に集中した。

**目的**: big-bang の前に、サイレントなデータ消失を機械検出する安全網と、レジストリの中立ヘッダ基盤を用意する。挙動は一切変えない。

**触るファイル**:
1. `include/engine/ecs/ComponentRegistry.h`（新規）: `RuntimeComponentInfo` / `RuntimeComponentRegistry` / `FieldDef`（variant 版）。具象コア型を include しない薄い中立ヘッダ。
2. `src/ecs/EngineComponents.def`（新規）: 既存コア部品（Transform/MeshRenderer/Camera/各Light/RigidBody/各Collider/Audio/Skeletal/NodeAnim/NameTag）を列挙。**この時点ではまだ未使用**（次フェーズで配線）。
3. `src/ecs/ComponentReflect.h`（新規）: `FIELD/FIELD_ENUM/FIELD_RANGE` マクロ（offsetof ベース軽量リフレクション）。
4. `tests/serialize_roundtrip_test.cpp`（新規）: 現行 `SceneSerializer` で全コア部品を serialize→deserialize→フィールド一致を検証する round-trip テスト。**現行コードに対して先に通す**（移行後のサイレント欠落を機械検出する基準線）。
5. `CMakeLists.txt`: `include/` を公開ヘッダ用ディレクトリとして追加（既存の `src/..` 公開はまだ残す＝非破壊）。

**完了条件**:
- round-trip テストが現行実装でグリーン。
- `ComponentRegistry.h` が `EngineRuntime` 候補のどのターゲットからもジャンル型 include 無しでコンパイルできる。
- testengine の3サンプルが従来どおり動く（挙動不変）。

**サンプル担保**: コード経路を一切変えていないため自明に動く。

### フェーズ1 — コア部品をレジストリへ（シリアライズ脱ハードコード）

> **🟡 進行中 (2026-06-22) — レジストリ機構確立＋恒久コア8部品を移設（両構成グリーン・往復テスト同値）**
> - `src/scene/ComponentRegistry.cpp`（新規）: `RuntimeComponentRegistry`（typeName→serialize/deserialize/applyOverride/postLoad）の実装。`src/scene/CMakeLists.txt` に追加＋`include/` を公開ヘッダに。
> - `src/scene/SceneSerializer.cpp`: `RegisterCoreComponentSerializers()` を追加し、`SerializeEntityJson`/`InstantiateEntityJson` の `if(all_of<T>)` / `if(contains)` 連鎖を `RuntimeComponentRegistry::Get().ForEach(...)` 走査へ置換。**既存コードをそのままラムダ移設＝挙動不変**。
> - 移設済み（8）: PointLight / DirectionalLight / SpotLight / CameraComponent / RigidBody / BoxCollider / SphereCollider / CapsuleCollider ＝ エンジンに恒久的に残る空間・物理・描画コア。
> - 検証: debug+release ビルド成功、`ctest`（IndexAllocator + SerializeRoundtrip）グリーン。
> - **残り（意図的に後続）**: Gimmick / Trigger は **Phase 3 でエンジンから撤去**されるため registry へは移さず inline 据え置き（移設は捨て仕事）。AudioSource / ParticleEmitter / ConvexHullCollider / LuaScript は registry パターンが確立済みのため各フェーズで漸進移設。
> - **未着手（Phase 1 残タスク）**: `entt::meta` によるコア部品フィールド反映（Phase 2 の Inspector 自動 UI で初めて消費するため Phase 2 で実装）、stable-id(UUID) 足場。

**目的**: `SceneSerializer` の `if(all_of<T>)` 3連鎖（Serialize/Instantiate/ApplyOverrides）をレジストリ走査に置換。コア部品の serialize/deserialize/applyOverride/postLoad ラムダを登録関数へ移設（挙動不変）。

**触るファイル**: `src/ecs/ComponentRegistry.cpp`（新規・コア登録）、`src/scene/SceneSerializer.cpp`（3関数をレジストリループ化）、`ConvexHullCollider` は `postLoad` で Mesh 依存再生成、`Material`/primitive は custom-serialize ラムダ。

**完了条件**: round-trip テスト継続グリーン（移設ミスのサイレント欠落をここで捕捉）。既存 scene.json がそのまま読み書きできる。3サンプル動作。

### フェーズ2 — ScriptComponents 複数化 + L2 部品宣言 + Inspector 脱結合

**目的**: `LuaScript`(単一) を `ScriptComponents`(vector) へ。`properties={}` を `component{}` に発展。Inspector を「選択エンティティの全 descriptor を回す単一ループ」へ。EditorComponentRegistry（2層化）。SnapshotUndo（可変長 props 対応、コア POD は memcmp 高速パス併存）。

**触るファイル**: `Components.h`（LuaScript→ScriptComponents）、`ScriptEngine.{h,cpp}`（5ライフサイクル+Inject を ScriptInstance 反復へ、1マネージャに集約）、`InspectorPanel.{cpp,h}`（描画ブロック群→単一ループ、EditState 型消去、Enum combo 追加）、`UndoSystem.h`（SnapshotEditCommand）、`HierarchyPanel.cpp`（Add メニュー動的生成、PickEntityIcon 重複削除→descriptor メタ）。

**完了条件**: 既存 Spinner.lua が新形式で動く。Health+Inventory を同一エンティティに貼れる。ホットリロードのマージ規則（§2.3.4）が効く。Lua props 編集に Undo が効く。3サンプル動作（Spinner 回転を維持）。

### フェーズ3 — prelude 外出し + EventBus + Trigger 縮退 + IGameServices

**目的**: `kPrelude` を `assets/lib/*.lua` へ全移設し `LoadLibScripts()` で globals へ読む（`lua.globals()` フォールバック契約温存）。`events` を C++ EventBus バインドへ。`TriggerActionType` switch 撤去 → overlap→`events:emit`。`Gimmick` 完全削除。6コールバック → IGameServices。**後方互換マイグレータを先行実装**（旧 trigger.actions int type / gimmick.kind / particleEmitter.kind を検出し対応 Lua 部品へ機械変換、`schemaVersion` 導入、変換不能はエラー＝サイレント破棄禁止）。

**触るファイル**: `ScriptEngine.{h,cpp}`（kPrelude撤去/LoadLibScripts/UpdateTriggers置換/gimmicks撤去/events→EventBus/game.*）、`Components.h`（Gimmick削除、Trigger縮退）、`EventBus.{h,cpp}`/`IGameServices.h`（新規）、`SceneSerializer.cpp`（legacy 変換シム + schemaVersion）、`main.cpp`（type==7/8 撤去→中立検証）、`Application.{cpp,h}`（WireScriptCallbacks→IGameServices 1本、pendingSpawns の __gimmick_*__/__trigger__ 撤去）、`assets/lib/*.lua`（topdown/camera/fx/flow/keys/trigger_actions）、`assets/components/{Region,Mover,Hazard,SpikeTrap,MovingWall}.lua`（新規）。

**完了条件**: 旧 prelude 依存サンプルが同名 API のまま無改造で動く（挙動同等性テスト：level1 の半径0.5×scale XZ AABB 壁スライド等）。旧 scene.json（int enum）がマイグレータ経由で読め、新形式で再保存される。3サンプル動作。

### フェーズ4 — 能力カタログ実装（2D描画 + 空間クエリ + 接触 + Tag + 入力 + 時間）

**目的**: §3 の不足プリミティブを一括実装。これがあって初めて 2D/非リアルタイム/RTS が「作れる」状態になる。

**触るファイル**: `Camera.{h,cpp}`（ortho）、`SpriteRenderer.{h,cpp}` + Sprite HLSL（ECS駆動ワールド空間/depthソート）、`PhysicsSystem.{h,cpp}`（overlap*/onContact/step/setPaused/SetGravity/ContactListener）、`Scene`（SpatialIndex + `query`/`spatialQuery`）、`Components.h`（Tag/Sprite2D/DataComponent + Camera ortho フィールド）、`InputSystem.{h,cpp}` + `ActionMap.{h,cpp}`（全マウス/ホイール/絶対座標/bindAction/pickEntity、キャプチャ オプトイン）、`Animator/NodeAnimator`（onFinish）、`Application.cpp`（時間モデル更新ストラテジ化、null 安全化、audio リスナー脱結合）、`ScriptEngine.cpp`（新バインド群、EngineServices/ProjectRuntimeConfig）。

**完了条件**: ortho カメラで 2D スプライトがワールド空間に描ける。`scene:spatialQuery` が grid で範囲列挙。`physics:onContact` がイベントを流す。`time:pause/step` でターン制が成立。新規ジャンル最小サンプル（2D見下ろし・ボード）が **L3 のみ** で動く。3サンプル動作（既存3Dは無回帰）。

### フェーズ5 — RTS 性能パス（POD ミラー + 汎用 system + onUpdateBatch）

**目的**: 数千エンティティのホットパスを Lua 越えから外す。

**触るファイル**: `ScriptEngine.cpp`（`onUpdateBatch` バッチ呼び）、`src/ecs/`（POD ミラー部品の entt 動的 storage）、`src/sim/Mover.cpp`/`SpatialQuerySystem.cpp`（新規・C++ 汎用 system、entt view 一括）。

**完了条件**: 数千 Mover が C++ system で回り、Lua はイベント時のみ介在。プロファイルで Lua 境界越えがホットパスから消える。RTS 最小サンプルが L3 のみで動く。

### フェーズ6 — exe 分割 + テンプレ/プロジェクト データ化 + 封印確定

**目的**: 封印ランタイム exe を独立させ、自己診断を恒久 CI 化。

**触るファイル**: `CMakeLists.txt`（EngineRuntime INTERFACE / dx12-runtime / dx12-editor、include 隔離で `src/..` 公開撤廃）、`src/runtime_main.cpp`（新規）、`src/project/ProjectData.*`（GUI/git からデータ分離）、`Project.cpp`（C文字列テンプレ撤去→`templates/<id>/` ディスク読み）、`ProjectManager.cpp`（テンプレ走査で動的ボタン）、`Application.cpp`（BuildGame が runtime exe をコピー、newSceneTemplate データ化）。

**完了条件**: `dx12-runtime.exe` が Editor 非リンクでゲームを起動。BuildGame が runtime をコピー。`templates/` 追加だけで新ジャンルテンプレが増える。**自己診断 CI（§9）がグリーン**。3サンプル + 新規4ジャンルサンプルが全て動く。

---

## 6. 性能方針（大量エンティティ）

レビューが繰り返し指摘した通り、毎フレーム N 体の Lua `OnUpdate` 越えと overlap のヒープ確保は RTS で破綻する。対策を **設計の必須要件** として固定する。

1. **3層のホットパス委譲**:
   - 軽量: `onUpdateBatch(entities, dt)` — 同一 descriptor の全インスタンスを 1 回の Lua 呼びでまとめる（N→1 境界削減）。
   - 中量: **POD ミラー部品** — 性能が要る数値だけ descriptor が宣言し、C++ 側 entt 動的 storage（SoA）に置き、汎用 `Mover`/`SpatialQuery` system が view で一括処理。Lua はイベント時のみ。
   - 重量: 群シミュ自体を C++ 汎用 system（`SpatialIndex` 駆動）として v1 実装。ゲームは Tag + データで指令。
2. **空間クエリのアロケーション排除**: overlap は `out-param + cap` で事前確保バッファ + span 返却。Lua へは必要時のみ table 化。
3. **Jolt 上限のデータ化**: `maxBodies/maxBodyPairs/maxContactConstraints/gravity` を `project.runtime` 化。上限超の群は body 非登録 + `SpatialIndex`。
4. **Trigger の C++ バッチ評価**: 全 Region を C++ が 1 パスで enter/exit/stay 解決し、状態遷移のみ `Post`。Lua から毎フレーム overlap を呼ばせない。`ComputeWorldMatrix` はダーティフラグでフレーム1回キャッシュ（静的トリガは初回のみ）。
5. **EngineEvent の軽量ペイロード**: `unordered_map×2` ではなく小 vector / SoA。フレーム末に数万イベントでもヒープ確保を抑制。
6. **エンティティ参照の stable-id**: 名前文字列（`FindEntityByName` 線形走査）を stable-id(UUID) へ。シリアライズは id、表示のみ名前。RTS の同名衝突/複製/動的スポーン破綻を解消（§8 マイグレーションで全シーン再保存）。

---

## 7. 残る穴（v1 後もエンジン改造が要るジャンル能力）

設計の射程を正直に開示する。以下は本 big-bang の **スコープ外** であり、別フェーズ/別タスクとして並走する前提。

### 7.1 2D サブジャンルのレンダラ語彙
横スク視差/パララックス、等角(アイソメ)trueソート、ヘックス座標、ピクセルパーフェクトスナップ、オートタイル/タイルアニメ、9-slice は、HLSL+頂点レイアウト+PSO の同期編集を伴う。**緩和策**: 2D 描画を「データ駆動マテリアル(shaderId+named params) + 汎用 instanced-quad + カメラ/マテリアル のデータパラメータ(ソート/スナップ)」に寄せれば多くを吸収できるが、これ自体が §7.5 のマテリアル一般化に依存。

### 7.2 RTS の超大規模・群シミュの拡張
v1 で `SpatialIndex` + 汎用 Mover/SpatialQuery を入れるが、フォーメーション/ステアリング/フロックの**新しいアルゴリズム**を足すのは C++ system 追加（= src 改造）。

### 7.3 入力デバイス拡張
ゲームパッド(XInput)、タッチが `InputSystem` に皆無。v1 では全マウスボタン/ホイール/絶対座標/アクションマップ/ピッキングまで。パッド対応は src 改造。

### 7.4 マルチカメラ/複数ビューポート
分割画面・ミニマップ・PinP はレンダラが単一 view/proj 前提。v1 の `projection` 追加では解決せず、レンダラのマルチビューポート対応 + audio リスナー複数化が別途必要。

### 7.5 3D 見た目バリエーション（レンダリングのデータ駆動化）
`Material` 固定3スロットPBR、`ParticleKind`(8)/`BeamKind`(3)/`PostEffectBit`(25)/`TransitionType`(4) は C++/HLSL/頂点レイアウト/sol2 の4点同時同期。トゥーン/アンリット/カスタムシェーダ/新パーティクル見た目/新ポスト/スケルタル ステートマシン・アニメイベント層は **component{} では登録不能**。別フェーズで「シェーダID化 + データ駆動マテリアル + ポストチェーンのデータ化」を設計する。

---

## 8. リスクと後方互換

### 8.1 主要リスクと対策

| リスク | 対策 |
|---|---|
| `lua.globals()` フォールバック契約を壊すと全アタッチスクリプト即死 | lib 外出しは「sol::state 生成のたびに必ず通る初期化フェーズ」に位置づけ、OnPlayStart/RebuildScene/DoRuntimeSceneLoad の全 sol 再Init経路で再ロード保証（WireScriptCallbacks の「再注入忘れ」パターンを踏まない） |
| serialize ラムダ移設のサイレント欠落 | フェーズ0で round-trip テストを先行導入し全フェーズで継続。`applyOverride` を第一級メンバ化、`postLoad` で順序依存（ConvexHull）を表現 |
| ScriptComponents 複数化が Play/Stop/ホットリロードと不整合 | ライフサイクルを1マネージャに集約（3重複解消）。マージ規則を明文化（§2.3.4） |
| Trigger event 化でノーコード体験喪失 | 標準同梱 `ActionTable.lua`（イベント→中立アクション表）+ descriptor 編集の二段構え |
| include 境界の漏れで封印が規約頼み | `include/engine/` のみ公開、`src/..` 全公開撤廃。CI で内部ヘッダ跨ぎ include を検出 |
| physics=null 時に overlap/raycast/onContact が落ちる | `scene:spatialQuery` 統一ファサードに一本化、戻り値型を常に保証（physics無しでも空table/hit=false）。AABB サイズは Bounds/Sprite2D/Mesh 由来で確定 |
| 2D レンダラ作り直しコスト | フェーズ4で集中。SpriteRenderer は HUD 専用に割り切り、ECS 2D は instanced-quad に寄せる |

### 8.2 既存 scene/lua の移行（マイグレーション＝実装必須項目）

- **schemaVersion** を scene.json に導入。ロード時に旧→新変換するマイグレータを `SceneSerializer` 前段に**常設**（openQuestion ではなく必須）。
- 変換表（テスト付きで列挙）: `gimmick.kind`(int)→`Mover.lua`/`Hazard.lua`+props、`trigger.actions[]`(int type 0..10)→`Region.lua`+`enterEvent` + `trigger_actions.lua` ハンドラ、`particleEmitter.kind`(int)→プリセット名、`GridPlane`→マテリアルフラグ、`rigidBody.motionType`(int)→文字列、`filter`空→旧既定 `Player` を明示補完（誤発火回避）。
- 合成イベント名は entityidx ではなく **stable-id** で（プレハブ/MakeUniqueName でのズレ防止）。
- 変換不能ケースは**明示エラー**（サイレント破棄禁止）。
- 既存サンプル（title/level1/clear）は prelude 同名 API を `assets/lib` に移植 + Trigger 未使用のため遷移/HUD/イベントは後方互換エイリアスで生存。
- entity 参照 stable-id 化に伴う全シーン再保存はこのマイグレータ経由で one-shot 実行。

---

## 9. 自己診断テスト（src/ からゲーム固有概念を全消ししてもビルドが通る）

**どのフェーズで満たすか**: 段階的に近づき、**フェーズ6 で恒久 CI として確定**する。

- フェーズ3完了時: `Gimmick|TriggerActionType|moveTopDown|cameraTPS|win|"Player"` が `src/{ecs,scripting,scene}` で grep 0件（語彙削除完了）。
- フェーズ4完了時: `src/{renderer,physics,input}` にジャンル語彙が無く、能力 API がジャンル中立名のみ。
- フェーズ6完了時（恒久）:
  1. `! grep -rEn 'Gimmick|TriggerActionType|GimmickKind|moveTopDown|cameraTPS|cameraLockOn|\bwin\b|"Player"|"Goal"|FX\.explosion' src/{ecs,scripting,scene,renderer,physics,input,core}` が 0件。
  2. `dx12-runtime.exe` が `EditorLib`/`ProjectLib` を**リンクせず**ビルド・起動できる（封印リンク境界）。
  3. round-trip シリアライズテストがグリーン（サイレント欠落なし）。
  4. 内部ヘッダ跨ぎ include 検出が 0件（公開ヘッダ隔離）。
  5. 4ジャンル最小サンプル（2D見下ろし/3Dアクション/ボード/RTS）が `assets/` のみ（src 無改造）で起動・動作。
  6. testengine の title/level1/clear が無改造で動作。

---

## 10. 未確定（ユーザー判断が要る点）

### 10.0 確定事項（2026-06-22 ユーザー決定）
- **entity 参照の同一性モデル = stable-id(UUID) を v1 から導入**（下記1を確定）。シリアライズは id、表示のみ名前。全シーンはフェーズ3のマイグレータ経由で one-shot 再保存。
- **コア部品リフレクション実装 = `entt::meta`**（下記3を確定）。`FieldDef`/Inspector は entt::meta のデータ/プロパティから生成。既に entt 依存のため追加依存なし。ビルド時間増は許容。
- **最初に完全動作させる新ジャンル = 2D/2.5D 見下ろし**。フェーズ4の能力実装は ortho カメラ → Sprite2D 描画 → 空間 overlap → Tag/入力アクションマップ の順を先頭化。フェーズ4末の最初の新ジャンル検証サンプルは「2D見下ろし最小ゲーム（assets/ のみ）」。

### 10.1 以降も継続検討
1. ~~**entity 参照の同一性モデル**~~ → 10.0 で UUID 確定。
2. **POD ミラー部品の v1 投入**: `onUpdateBatch` だけで様子を見るか、最初から POD ミラー（entt 動的 storage）を入れるか。RTS を対象に含むなら v1 必須と判断（本書は v1 想定）。
3. ~~**コア部品リフレクション実装**~~ → 10.0 で `entt::meta` 確定。
4. **GameComponent ストレージ**: 単一 `ScriptComponents` vector（実装軽・Clear整合）か typeName ごと entt 動的 storage（view 高速・RTS向け）か。RTS 対象のため後者を性能パス用に併用（§6）。
5. **exe 2分割のタイミング**: フェーズ6で完全分割（本書）か、当面単一 exe で「エディタ無しモード」代用か。CI/配布/BuildGame に波及。
6. **テンプレ標準ライブラリ**: 全プロジェクト共通 `std.lua` 1本か、ジャンル別パック（topdown/tps/board）を `project.stdlib` 配列で選ぶか。
7. **マルチカメラ/データ駆動マテリアル/ゲームパッド（§7.4,7.5,7.3）**: 別フェーズの優先順位。どのジャンルを最初に完全対応させたいかで決まる。
8. **ノーコード Trigger UI**: `ActionTable.lua` 二段構えで十分か、専用 GUI を作るか。
