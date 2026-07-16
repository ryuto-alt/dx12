# DX12 Engine — API リファレンス（完全版）

DX12 ゲームエンジン（C++20 / ECS=entt / Lua=sol2 / 物理=Jolt）の **公開 API 全一覧**。
ソース（`src/scripting/ScriptEngine.cpp` / `src/ecs/Components.h` / `src/main.cpp` / `docs/MCP.md`）から実体を抽出して構成している。

ゲームの中身は「データ（シーン JSON + `.lua`）」と「スクリプト API（Lua）」で作る。同じものを人間はエディタで、Claude はテキストで書ける。

- 関連: [`SCRIPTING.md`](SCRIPTING.md)（チュートリアル） / [`AUTHORING.md`](AUTHORING.md)（データ駆動） / [`SCRIPT_COMPONENTS.md`](SCRIPT_COMPONENTS.md)（プロパティ付きコンポーネント） / [`MCP.md`](MCP.md)（AI ブリッジ）

---

## 目次
1. [スクリプトのライフサイクル](#1-スクリプトのライフサイクル)
2. [グローバルオブジェクト一覧](#2-グローバルオブジェクト一覧)
3. [Lua usertype（型）リファレンス](#3-lua-usertype型リファレンス)
4. [グローバル関数・定数](#4-グローバル関数定数)
5. [高レベル prelude ヘルパー](#5-高レベル-prelude-ヘルパー)
6. [VFX / パーティクル API](#6-vfx--パーティクル-api)
7. [ECS コンポーネント全一覧](#7-ecs-コンポーネント全一覧)
8. [データ駆動オーサリング（Trigger / ParticleEmitter / プロパティ）](#8-データ駆動オーサリング)
9. [コマンドラインフラグ](#9-コマンドラインフラグ)
10. [MCP（AI ブリッジ）ツール一覧](#10-mcpai-ブリッジツール一覧)
11. [C++ 主要モジュール API（埋め込み / エンジン拡張向け）](#11-c-主要モジュール-api)

---

## 1. スクリプトのライフサイクル

### アタッチスクリプト（エンティティに貼る `.lua`）
エンティティの `LuaScript` コンポーネントに添付した `.lua` は、Play 開始時にインスタンス化される。

```lua
-- 公開プロパティ（Inspector で編集・シーンに保存・Play 時に self へ注入）
properties = {
  { name="speed", type="float", default=5.0, min=0, max=20, label="移動速度" },
  { name="target", type="entity", default="" },
}

function OnStart(self)            -- Play 開始時に 1 回
  -- self.entity   : エンティティ ID (u32)
  -- self.name     : 名前
  -- self.transform: Transform（position/rotation/scale を読み書き可）
  -- self.enabled  : 有効フラグ
  -- self.speed    : ↑ properties で宣言した値が注入される
end

function OnUpdate(self, dt)       -- 毎フレーム（dt=秒）
end
```

`self` に常に入るキー: `entity` / `name` / `transform` / `enabled` ＋ `properties` 宣言値。
スクリプト本体は `lua.globals()` をフォールバック環境とするため、**全グローバル（`scene` / `camera` / `input` / `audio` / `physics` / `events` / `ui` / `fx` / 各ヘルパー）にアクセスできる**。

### グローバルスクリプト（`game.lua`）
プロジェクト全体の `game.lua` はトップレベルの `OnStart()` / `OnUpdate(dt)`（`self` 無し）を使う。コンポーネント方式では任意。

### プロパティ型（`type=`）
`float` / `int` / `bool` / `string` / `vec3` / `color` / `entity`（他エンティティ名で参照、Play 時に `Entity` 解決）

---

## 2. グローバルオブジェクト一覧

| 名前 | 型 | 説明 |
|------|----|------|
| `scene` | Scene | エンティティの生成・検索・クエリ |
| `input` | Input | キーボード / マウス入力 |
| `camera` | Camera | アクティブカメラの操作 |
| `audio` | AudioSystem | BGM / SE / 3D 空間オーディオ |
| `physics` | PhysicsSystem | 剛体・コライダー・レイキャスト・キャラ移動 |
| `events` | table | 疎結合イベントバス（C++ EventBus の薄バインド） |
| `time` | table | 経過時間・タイムスケール（ポーズ/スローモ）・タイマー |
| `ui` | table | 即時モード ゲーム内 UI |
| `fx` | table | 即時パーティクル放出（burst/ring/beam/pulse） |
| `vfx` | table | 統一 VFX 窓口（コード or Effekseer） |
| `ASSETS` | string | assets ディレクトリの絶対パス |
| `SCREEN_W` / `SCREEN_H` | int | 画面解像度（`SetScreenSize` で更新） |

---

## 3. Lua usertype（型）リファレンス

### Vec3
`DirectX::XMFLOAT3` のラッパー。
```lua
local v = Vec3.new(x, y, z)   -- or Vec3.new()
v.x, v.y, v.z                 -- 読み書き
```

### Transform
```lua
t.position   -- Vec3
t.rotation   -- Vec3（Euler 度）
t.scale      -- Vec3
```

### Entity
| メソッド/プロパティ | 戻り値 | 説明 |
|---|---|---|
| `:isValid()` | bool | 有効なエンティティか |
| `.name` | string | 名前（読み取り専用プロパティ） |
| `.transform` | Transform | Transform 参照（読み書き） |
| `:hasComponent(type)` | bool | コンポーネント有無。type: `"Transform"`,`"MeshRenderer"`,`"SkeletalAnimation"`,`"NodeAnimation"`,`"GridPlane"`,`"PointLight"`,`"DirectionalLight"`,`"SpotLight"`,`"Camera"`,`"AudioSource"`,`"Gimmick"`,`"ParticleEmitter"`,`"Trigger"`,`"CharacterController"`,`"UICanvas"`,`"UIRect"`,`"UIImage"`,`"UIText"`,`"UIButton"`,`"UISlider"`,`"UIToggle"`,`"UIScrollView"`,`"UILayout"`,`"UIAnimator"` |
| `:playAnim(clipIndex, blendDuration)` | — | スケルタルアニメをクリップ番号で再生（クロスフェード） |
| `:playAnimByName(name, blendDuration)` | — | クリップ名で再生 |
| `:setLooping(loop)` | — | ループ ON/OFF |
| `:getAnimCount()` | int | クリップ数 |
| `:getAnimName(index)` | string | クリップ名 |

### Scene（`scene`）
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:spawn(name, modelPath, pos, rot, scale)` | Entity | モデルを生成（pos/rot/scale は Vec3） |
| `:spawnPlane(name, pos, size, grid)` | Entity | 平面を生成（grid=bool） |
| `:spawnBox(name, pos, rot, scale)` | Entity | ボックスを生成 |
| `:spawnSphere(name, pos, radius)` | Entity | 球を生成 |
| `:remove(entity)` | — | 削除 |
| `:getEntityCount()` | int | エンティティ総数 |
| `:findEntity(name)` | Entity | 名前で検索 |
| `:setUVScale(e, u, v)` | — | UV タイリング（生成時に1回。GPU 同期あり） |
| `:setColor(e, r, g, b)` | — | 頂点カラーで着色（生成時に1回。毎フレーム禁止） |
| `:setUiText(e, text)` | — | `UIText.text` を書き換える（無ければ何もしない）。タイプライター中なら先頭から再生し直す |
| `:getUiText(e)` | string | `UIText.text` を読む（無ければ空文字列） |
| `:setUiTypewriter(e, charsPerSec)` | — | タイプライター速度（文字/秒）を設定して先頭から再生。0=即全表示（スキップに使える） |
| `:isUiTypewriterDone(e)` | bool | タイプライターが全文まで到達したか（会話の「クリックで次へ」判定用。無効時は true） |
| `:setUiColor(e, r, g, b, a)` | — | `UIImage.color` 優先、無ければ `UIText.color`（どちらも無ければ何もしない） |
| `:setUiVisible(e, visible)` | — | `UIRect.visible` 優先（自身と子孫ごと隠す）、UIRect が無く UICanvas のみなら `UICanvas.visible` |
| `:setUiTexture(e, path)` | — | `UIImage.texturePath` を差し替え（assets 相対。無ければ何もしない） |
| `:setUiFill(e, amount)` | — | `UIImage.fillAmount`（表示割合 0..1、クランプあり）を設定。HPバー/ゲージ用 |
| `:getUiFill(e)` | number | `UIImage.fillAmount` を読む（無ければ 0） |
| `:setUiRotation(e, deg)` | — | `UIRect.rotation`（視覚回転・度・時計回り。ピボット回り、子孫も一緒に回る）を設定 |
| `:getUiRotation(e)` | number | `UIRect.rotation` を読む（無ければ 0） |
| `:getUiSlider(e)` | number | `UISlider.value`（実値 min..max）を読む（無ければ 0） |
| `:setUiSlider(e, v)` | — | `UISlider.value` を設定（min..max へクランプ。`onChangeEvent` は発火しない） |
| `:getUiToggle(e)` | bool | `UIToggle.isOn` を読む（無ければ false） |
| `:setUiToggle(e, on)` | — | `UIToggle.isOn` を設定（`onChangeEvent` は発火しない） |
| `:getUiScroll(e)` | x, y | `UIScrollView` のスクロール量(px)を2値で返す |
| `:setUiScroll(e, x, y)` | — | スクロール量を設定（コンテンツ量へ自動クランプ。大きい値で「一番下へ」） |
| `:tweenUi(e, params)` | — | UI をトゥイーンで動かす。`params={dx?,dy?,scale?,scaleX?,scaleY?,alpha?,rotate?,color?,shake?,shakeFreq?,fill?,countTo?,countFrom?,countFmt?,onComplete?,duration?,delay?,easing?}`。`dx/dy`=UIRect offset の相対移動(px)、`scale/scaleX/scaleY/alpha/rotate/color`=見た目のみ（子孫にも掛かる。`rotate` は度・絶対目標値で `UIRect.rotation` へ加算合成。`scaleX/scaleY` で非等方=フリップ/スカッシュ）。`color={r,g,b}`=RGB乗算の絶対目標値（1超えで白フラッシュ。完了後も持続={1,1,1}で解除）。`shake`=振幅px の振動（duration で 0 へ減衰。`shakeFreq`=Hz 既定24）。`fill`=UIImage.fillAmount の絶対目標値（ゲージなめらか増減。from は現在値）。`countTo`=UIText への数字ロール（`countFrom` 省略時は現在テキストの数値解釈、`countFmt`=printf 書式 `"%d"/"%05d"/"%.1f"`）。`onComplete`=完了時に1回呼ばれる関数（SE同期/チェーン）。`easing`: `"linear"/"in"/"out"/"inOut"/"back"/"bounce"/"elastic"/"expo"/"inBack"/"inOutBack"/"quint"/"sine"`。`e` は Entity かエンティティID |
| `:stopUiTweens(e)` | — | 進行中の tween を全打ち切り+視覚値リセット（DOTween の Kill 相当。連打対策） |
| `:showUi(e)` | — | 表示して `UIAnimator` の出現アニメを最初から再生（無ければ visible=true のみ） |
| `:hideUi(e)` | — | 出現アニメの逆再生で消す（子孫ごと。消えた後はクリック不可）。無アニメなら即非表示。戻すのは `showUi` |
| `:setSpriteEffect(e, value)` | — | Sprite2D の effectValue（カスタムシェーダーへの汎用値）を変更。毎フレーム可 |
| `:setSpriteAlpha(e, alpha)` | — | Sprite2D の不透明度(0..1)を変更。半透明演出用。毎フレーム可 |
| `:setSpriteParams(e, x, y, z, w)` | — | Sprite2D の shaderParams（カスタムシェーダーへの汎用 float4、TEXCOORD2）を変更。毎フレーム可 |
| `:setMeshEffect(e, value)` | — | MeshRenderer の effectValue（カスタムシェーダーへの汎用値）を変更。毎フレーム可 |
| `:setMeshParams(e, x, y, z, w)` | — | MeshRenderer の shaderParams（カスタムシェーダーへの汎用 float4）を変更。毎フレーム可 |
| `:gimmicks()` | table | Gimmick 付き全エンティティを配列で返す（要素: `{e,name,kind,period,phase,amplitude,threshold,solid,deadly}`） |
| `:queryByTag(tag)` | table | タグ一致エンティティの**名前配列** |
| `:queryInBox(minX, minZ, maxX, maxZ, tag?)` | table | XZ 矩形内のエンティティ名配列（RTS の矩形選択向け） |

### Input（`input`）
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:isKeyDown(vk)` | bool | 押されている間 true |
| `:isKeyPressed(vk)` | bool | 押した瞬間だけ true |
| `:isAsyncKeyDown(vk)` | bool | 非同期キー状態 |
| `:isMouseCaptured()` | bool | マウスキャプチャ中か |
| `:setMouseCapture(b)` | — | マウスキャプチャ ON/OFF |
| `:isRightMouseDown()` | bool | 右ボタン |
| `:getMouseDeltaX()` / `:getMouseDeltaY()` | float | マウス移動量 |

> `vk` には `KEY_*` 定数（§4）を渡す。文字列で扱う `keyDown("W")`（§5）も用意。

### Gamepad（`input`、Xbox コントローラー / XInput、`pad` = 0..3）
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:isPadConnected(pad)` | bool | 接続中か |
| `:getConnectedPadCount()` | int | 接続台数 |
| `:isPadButtonDown(pad, btn)` | bool | 押されている間 true |
| `:isPadButtonPressed(pad, btn)` | bool | 押した瞬間だけ true |
| `:isPadButtonReleased(pad, btn)` | bool | 離した瞬間だけ true |
| `:getPadLeftStickX/Y(pad)` / `:getPadRightStickX/Y(pad)` | float | スティック傾き（円形デッドゾーン適用済み、-1..1） |
| `:getPadLeftTrigger(pad)` / `:getPadRightTrigger(pad)` | float | トリガー押し込み量（デッドゾーン適用済み、0..1） |
| `:setPadVibration(pad, leftMotor, rightMotor)` | — | 振動（0.0..1.0。left=低周波・強、right=高周波・弱） |
| `:setPadVibrationTimed(pad, leftMotor, rightMotor, sec)` | — | sec 秒後に自動停止する振動（ワンショット向け） |

`btn` には `PAD_A PAD_B PAD_X PAD_Y PAD_LB PAD_RB PAD_BACK PAD_START PAD_LSTICK PAD_RSTICK PAD_DPAD_UP/DOWN/LEFT/RIGHT` 定数を渡す（値は `XINPUT_GAMEPAD_*` と同一）。
文字列名で済ます `padDown("A")` / `padPressed("RB")` / `padStick("left")` / `padVibrate(low, high, sec?)`（§5）も用意。
デッドゾーンは XInput 標準値（`XINPUT_GAMEPAD_{LEFT,RIGHT}_THUMB_DEADZONE` / `TRIGGER_THRESHOLD`）を使用し、スティックは軸別ではなく円形（radial）で処理（斜め入力が弱くならないよう）。

### Camera（`camera`）
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:moveForward(amt)` / `:moveRight(amt)` / `:moveUp(amt)` | — | 移動 |
| `:rotate(dyaw, dpitch)` | — | 回転 |
| `:getPosition()` / `:setPosition(v)` | Vec3 | 位置 |
| `:getYaw()` / `:getPitch()` / `:setYaw(v)` / `:setPitch(v)` | float | 向き |
| `:getMoveSpeed()` / `:setMoveSpeed(v)` | float | 移動速度 |
| `:getMouseSensitivity()` / `:setMouseSensitivity(v)` | float | マウス感度 |
| `:project(x, y, z)` | u, v, visible | ワールド座標→正規化スクリーン座標（左上原点 [0,1]）。3 値返し。頭上ダメージ表示等に |

### AudioSystem（`audio`）
| メソッド | 説明 |
|---|---|
| `:playBGM(path)` / `:stopBGM()` / `:pauseBGM()` / `:resumeBGM()` | BGM 制御 |
| `:playSFX(path)` | 2D 効果音 |
| `:playSpatial(path, x, y, z, minD, maxD, vol?, loop?)` | 3D 空間効果音 |
| `:stopAllSFX()` | 全 SE 停止 |
| `:setMasterVolume(v)` / `:setBGMVolume(v)` / `:setSFXVolume(v)` | 音量設定 |
| `:getMasterVolume()` / `:getBGMVolume()` / `:getSFXVolume()` | 音量取得 |
| `:getBGMList()` / `:getSFXList()` | ファイル一覧 |
| `:rescan()` | オーディオフォルダ再スキャン |

### PhysicsSystem（`physics`）
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:autoCollider(e)` | — | メッシュ頂点から Convex Hull を自動生成（既存コライダーがあればスキップ・最大256頂点） |
| `:addBoxCollider(e, hx, hy, hz)` | — | ボックスコライダー追加 |
| `:addSphereCollider(e, radius)` | — | 球コライダー追加 |
| `:addCapsuleCollider(e, radius, halfHeight)` | — | カプセルコライダー追加 |
| `:addRigidBody(e, motionType, mass)` | — | 剛体追加（motionType=`MOTION_*`） |
| `:removeRigidBody(e)` | — | 剛体削除 |
| `:applyForce(e, vec3)` | — | 力を加える |
| `:applyImpulse(e, vec3)` | — | 撃力を加える |
| `:setVelocity(e, vec3)` / `:getVelocity(e)` | — / Vec3 | 速度 |
| `:setPosition(e, vec3)` | — | 物理位置を設定 |
| `:raycast(origin, dir, maxDist)` | RaycastHit | レイキャスト |
| `:overlapBox(center, half, maxN?=32)` | table | 範囲内エンティティ配列 |
| `:overlapSphere(center, radius, maxN?=32)` | table | 範囲内エンティティ配列 |
| `:setPaused(b)` | — | 物理ステップ停止/再開 |
| `:step(dt)` | — | 手動 1 ステップ（駒送り） |
| `:setGravity(vec3)` | — | 重力ベクトル設定 |
| `:addCharacterController(e, radius, halfHeight)` | — | キャラコン追加（RigidBody と排他） |
| `:move(e, vx, vz)` | — | 水平移動入力（world XZ 目標速度・毎フレーム呼ぶ） |
| `:jump(e, amount?)` | — | ジャンプ（接地中のみ・amount<=0 で既定 jumpSpeed） |
| `:isGrounded(e)` | bool | 接地判定 |

### RaycastHit
```lua
hit.hit       -- bool
hit.distance  -- float
hit.point     -- Vec3
hit.normal    -- Vec3
```

### events（`events`）— イベントバス
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:on(name, fn)` | id(uint) | 購読。`fn(data)` の data はテーブル。**Play 中のみ有効。`OnStart()` 内で登録すること** |
| `:off(id)` | — | 個別解除（`on` が返した id） |
| `:emit(name, data?)` | — | 発火。data は `{key=val,...}`（num/str/bool） |
| `:clear()` | — | 全購読解除（Play 開始時に自動 clear） |

> Trigger の `EmitEvent` アクション（§8）も C++ からこの `emit` を呼ぶ。物理接触は `engine.contact.enter` / `engine.contact.exit` を Post する。

### net（`net`）— オンラインマルチプレイ（リッスンサーバー方式・ENetベース、開発中）
| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:host(port?)` | string(err) | リッスンサーバー開始（自分もプレイヤー参加）。省略時 `network.json` の既定ポート。空文字列=成功 |
| `:join(ip, port?)` | string(err) | サーバーへ接続開始。**非同期**（成立/失敗は `net.clientConnected`/`net.disconnected` イベントで分かる） |
| `:disconnect()` | — | 切断 |
| `:isServer()` / `:isClient()` / `:isConnected()` | bool | 状態確認 |
| `:localClientId()` | int | 自分の clientId（ホストは常に0） |
| `:players()` | `{id, rtt}[]` | 接続中プレイヤー一覧 |
| `:spawn(prefabPath, x, y, z, owner?)` | netId(int), string(err) | **サーバー専用**。prefabPath は assets 相対（例 "prefabs/Player.prefab"）。生成はフレーム境界(`net.spawned`で分かる) |
| `:despawn(entity)` | string(err) | **サーバー専用**。NetworkIdentity付きエンティティを破棄(即時) |
| `:findByNetId(netId)` | Entity | netIdからエンティティを引く。見つからなければ`isValid()==false` |
| `:rpc(name, ...)` | string(err) | **クライアント専用**。サーバーへRPC送信。引数は number/string/boolean/Vec3 のみ |
| `:rpcAll(name, ...)` | string(err) | **サーバー専用**。全クライアントへRPC送信 |
| `:rpcClient(clientId, name, ...)` | string(err) | **サーバー専用**。特定クライアントへRPC送信 |
| `:onRpc(name, fn)` | — | RPCハンドラ登録。`fn(sender, ...)`。senderはクライアント受信時は常に0(サーバー) |
| `:setInput{moveX=,moveZ=,aimYaw=,aimPitch=,buttons=,jump=}` | — | **クライアント専用**。毎フレーム(OnUpdate)呼ぶ想定。全フィールド省略可(既定0/false) |
| `:getInput(entity)` | `{moveX,moveZ,aimYaw,aimPitch,buttons,jump}` | **サーバー専用**。entityのNetworkIdentity._ownerの最新入力を読む |

> イベント: `events:on("net.hostStarted"|"net.clientConnected"|"net.clientDisconnected"|"net.connected"|"net.clientReady"|"net.spawned"|"net.despawned"|"net.disconnected", fn)`。
> `net.clientConnected`(サーバー視点、低レベル接続) / `net.clientReady`(サーバー視点、Baseline適用済み=対戦準備完了) / `net.connected`(クライアント視点、Welcome受信=`localClientId()`確定) の data に `client`(clientId) が入る。
> `net.spawned` の data に `netId`/`owner`、`net.despawned` の data に `netId` が入る。
> `NetworkIdentity` コンポーネント（Inspectorの「Network Identity」）を付けたエンティティが複製対象。サーバーが `_netId` を採番し、クライアント接続時に Welcome(clientId+シーンパス)→Baseline(netId対応表)→SceneReady のハンドシェイクで同期する。動的スポーンは `.prefab` を both サイドで個別に `InstantiatePrefab` するため通信量が小さい(JSON blob を流さない)。
>
> `NetworkTransform` コンポーネント（Inspectorの「Network Transform」）を NetworkIdentity と併用すると、サーバーが Transform(位置/回転/スケール、フィールドごとにon/off可)を `snapshotRate`(network.json、既定20Hz)で全readyクライアントへ送信する。`syncMode=0`(補間、既定): クライアントは `interpDelayMs`(既定100ms)だけ過去を描画するよう2点補間(位置lerp・回転slerp)して書き込む。`syncMode=1`(オーナー予測、自分がownerのCharacterController付きエンティティ向け): クライアントは自分の入力を即座にローカル物理へ反映しつつ(体感の遅延ゼロ)、サーバーから来た確定位置と自分の同tick時点の予測位置を比較し、`snapDistance`を超えて食い違っていたらサーバー位置へテレポート→未確認の入力だけ`PhysicsSystem::StepSingleCharacter`で再適用して「今」まで追いつく(古典的なclient-side prediction + reconciliation)。誤差が閾値以内ならローカル予測をそのまま信頼し補正しない。
> 入力コマンド: クライアントの`net:setInput`は直近3フレーム分を冗長送信(unreliable、パケットロス耐性)し、内部的には最大64件(`kInputHistoryCap`)の入力履歴を保持してリプレイに使う。サーバーは`net:getInput(entity)`で最新値(tickが一番新しいもの)を読み、`physics:move(entity, input.moveX, input.moveZ)`等ゲームスクリプト側で実際の移動に反映する想定(入力の適用方法はエンジンでなくゲームロジックが決める)。
>
> 興味管理(interest management): `NetworkIdentity.interestRadius`(0=常に関連)を使い、サーバーは各クライアントの所有エンティティ位置を観測点として、半径を超えて離れた複製エンティティのTransformスナップショットをそのクライアントへは送らない(帯域のO(接続数×エンティティ数)爆発を回避)。所有物が無いクライアントにはフィルタせず全部送る。現状は「継続的なスナップショット送信」だけを興味管理の対象にしており、動的スポーン(`net:spawn`)は引き続き全クライアントへブロードキャストする(スポーンは頻度が低く帯域上の主要因ではないため)。
> エディタの「ツール > Network」窓(Play中)でロール/tick/複製数と接続一覧(clientId・RTT・送受信バイト概算)を確認できる。
> 現状はフェーズ⑧(興味管理+統計窓)まで実装済み。エディタ設定UI・セッション抽象は今後のフェーズで追加予定。

### time（`time`）— 時間 API（v0.9.3+）
`:` ではなく `.` で呼ぶ。状態は Play 開始でリセットされる。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.now()` | float | Play 開始からの経過秒（タイムスケール適用済み） |
| `.realtime()` | float | 実時間の経過秒（スケール非適用） |
| `.dt()` / `.realDt()` | float | 今フレームの dt（スケール済み / 実時間） |
| `.frame()` | int | フレームカウンタ |
| `.setScale(s)` / `.getScale()` | — / float | タイムスケール。0=ポーズ、0.5=スローモ、2=早送り |
| `.after(sec, fn)` | id | sec 秒後に fn を1回実行（スケール済み時間で進む） |
| `.every(sec, fn)` | id | sec 秒ごとに fn を繰り返し実行 |
| `.cancel(id)` | — | `after` / `every` の解除 |

> `setScale` は **`OnUpdate` に渡る dt 自体に掛かる**ので、既存スクリプトは無改修でスローモ/ポーズに追従する（物理・パーティクルは対象外）。ポーズ中も UI 操作を続けたい処理は `time.realDt()` を使う。

```lua
time.setScale(0.3)                          -- スローモーション
time.after(2.0, function() boss:roar() end) -- 2秒後に1回
local id = time.every(1.0, spawnEnemy)      -- 毎秒スポーン
time.cancel(id)
```

#### time.video — 共有ビデオ時計（決定論タイムライン）
ステージ全体に1本流れる"動画時間"。ギミックは `t = time.video.localTime(self)` を使って
**動きを t の純関数**で書く（サイン往復・パス移動など）。対象ごとのオフセットを ± するだけで
先送りも巻き戻しも正確に効く。エンティティのキーは self テーブル / 名前文字列 / 数値 id
（名前優先。同名エンティティは同一時計になる）。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.start(duration, {skipCost=1.0}?)` | — | ビデオ開始（例: 10秒）。`skipCost` は skip 時の残り時間消費倍率（0=消費なし） |
| `.stop()` / `.active()` | — / bool | 停止 / 動作中か |
| `.now()` / `.duration()` | float | 動画時間 / 全長 |
| `.remaining()` | float | 残り時間（skip の消費込み。未 start なら `math.huge`） |
| `.finished()` | bool | 残り時間が尽きたか（ゲームオーバー判定） |
| `.skip(ent, ±sec)` | offset | **対象だけ**先送り / 巻き戻し。残り時間を `|sec| * skipCost` 自動消費 |
| `.localTime(ent)` | float | `now() + 対象のオフセット`。ギミックの t はこれ |
| `.setOffset(ent, off)` / `.getOffset(ent)` | — / float | オフセットの直接操作 |

```lua
-- ステージ側
time.video.start(10, { skipCost = 1.0 })     -- 10秒の動画。先送りすると制限時間が減る
if time.video.finished() then gameOver() end
ui:text(20, 20, ("残り %.1f"):format(time.video.remaining()))

-- ギミック側（動きは t の純関数で書く）
function OnUpdate(self, dt)
  local t = time.video.localTime(self)
  self.transform.position = Vec3.new(self.bx, self.by + math.sin(t) * 2, self.bz)
end

-- 矢が刺さったら
time.video.skip("SpikeWall", 1.0)            -- 1秒先送り（巻き戻しは -1.0）
```

#### 個別時計 — エンティティ単位の独立クロック
ビデオ時計と無関係に、オブジェクトごとに進む・止まる・スキップできる時計。初アクセスで t=0 から開始。

| メソッド | 説明 |
|---|---|
| `time.localTime(ent)` | 対象の経過秒 |
| `time.skipEntity(ent, ±sec)` | 対象の時計を ± |
| `time.scaleEntity(ent, s)` / `time.getEntityScale(ent)` | 対象だけスロー / 停止(0) / 逆再生(負) |
| `time.resetEntity(ent)` | 時計を破棄（次アクセスで t=0） |

#### charge — 押しっぱなしチャージ計測（弓を引く等）
| メソッド | 説明 |
|---|---|
| `charge.new(key, {max=2, rate=1, realtime=false}?)` | 生成。`key` は `"E"` 等のキー名。`realtime=true` でポーズ中も実時間で溜まる |
| `c:update()` | `OnUpdate` で毎フレーム呼ぶ |
| `c:charging()` / `c:value()` / `c:ratio()` | チャージ中か / 現在量 / 0..1（ゲージ表示用） |
| `c:released()` | **離した瞬間だけ**チャージ量を返す（それ以外は nil）。返すと 0 にリセット |

```lua
local c
function OnStart(self) c = charge.new("E", { max = 2.0 }) end
function OnUpdate(self, dt)
  c:update()
  if c:charging() then ui:rect(20, 60, 200 * c:ratio(), 12, 1, 0.8, 0.2, 1) end
  local v = c:released()
  if v then shootArrow(self, v * self.skipPerCharge) end  -- 引いた量でスキップ量が変わる
end
```

### ui（`ui`）— 即時モード UI（簡易/デバッグ用）
`OnUpdate` 内で毎フレーム呼ぶと描画される（Play 中のみ）。座標指定で毎フレーム呼ぶだけの簡易 API。
恒常的な画面（タイトルメニュー・HUD 一式）を組むなら次項の「ゲーム内UI（コンポーネント方式）」を推奨。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `:text(x, y, text, size?=24, r?, g?, b?, a?)` | — | テキスト描画 |
| `:button(x, y, w, h, label)` | bool | ボタン（押下で true） |
| `:image(x, y, w, h, path)` | — | 画像描画 |
| `:rect(x, y, w, h, r?, g?, b?, a?, rounding?)` | — | 塗りつぶし矩形（バー/背景） |

### ゲーム内UI（コンポーネント方式・retained-mode）
Unity uGUI / Godot Control 相当の retained-mode UI。UI要素は **ECSコンポーネント**としてシーンに置き、
エディタで編集・シーン JSON に保存する（Hierarchy 右クリック→「作成」→「UI（ゲーム内UI）」で Canvas/Image/Text/Button を配置）。
レイアウトは**アンカー＋ピボット**で解像度追従し、描画は既存の即時 `ui:*` と同じ全画面オーバーレイに統合される
（retained UI が奥、即時 `ui:*` が手前）。表示条件は同じく **Play中 / ゲームモードのみ**（エディタ編集中の WYSIWYG プレビューは無し）。

| コンポーネント | 主なフィールド | 説明 |
|---|---|---|
| `UICanvas` | `refWidth/refHeight`(既定1920x1080), `scaleMode`(0=ScaleToFit 1=ConstantPixel), `sortOrder`, `visible` | UIツリーのルート |
| `UIRect` | `anchorMin/Max`, `pivot`, `offsetMin/Max`, `visible`, `rotation`(度), `skewX`(度), `clipChildren` | レイアウト矩形（RectTransform相当）。全UI要素に必須。`rotation/skewX` はレイアウト解決後にピボット回りへ掛かる**視覚変換**（子孫も一緒に回る。斜め配置パネル/平行四辺形バナー用。回転中はエディタのリサイズハンドル非表示=数値編集。`UIScrollView` ノード自身では無視）。`clipChildren=true` で子ツリーを自矩形で**マスク**（ワイプ公開/マーキー。ScrollView 同様このノード自身の回転は無効） |
| `UIImage` | `texturePath`(空=単色), `color`, `shape`(0=矩形 1=楕円 2=リング 3=ダイヤ 4=六角形 5=三角形)+`ringThickness`, `uvMin/Max`(1超え=タイル), `uvScroll`(uv/秒), `sliceBorder`(px 左上右下, 9-slice), `cornerRadius`, `raycastBlock`(既定true), `fillAmount`(0..1 既定1), `fillDir`(0=左 1=右 2=下 3=上から 4=放射時計回り 5=放射反時計回り)+`fillOrigin`(開始角度), `segments`+`segmentGap/Color`(分割ゲージ), `gradientDir`(0=なし 1=横 2=縦 3=斜め 4=放射)+`gradientColor2`+`gradientScrollSpeed`(周回/秒。0=静的), `outlineWidth`+`outlineColor`+`outlineStyle`(0=実線 1=破線 2=コーナーブラケット)+`outlineDash`, `shadowColor`(α0=無効)+`shadowOffset`+`shadowSoftness` | 画像/単色。`shape≠0` は**形状描画**＝テクスチャを形で切り抜く（丸アイコン/バフ枠/スキルノード。角丸/9-slice 無視。リングは単色専用の帯円=円形ゲージ）。放射 fill はクールダウン円（矩形にも効く。リングの fill は常に円弧）。`segments` はスタミナ/弾数のチャンクゲージ（矩形+線形fill専用）。`uvMax>1`+`uvScroll` で**流れるタイルパターン**（警告帯/ストライプ。9-sliceでは無効）。グラデは全描画形態に掛かる（`gradientColor2` のαは無視）。`gradientScrollSpeed≠0` で光帯がグラデ方向へ流れる（ガチャの光沢流し）。ブラケット枠は SF/照準 HUD の四隅鉤括弧。影は形状近似のドロップシャドウ |
| `UIText` | `text`, `fontSize`, `color`, `alignH/V`(0=左/上 1=中央 2=右/下), `wrap`, `outlineWidth`+`outlineColor`(縁取り), `shadowColor`(α0=無効)+`shadowOffset`(影), `fontPath`(assets相対 .ttf/.otf。空=既定Yu Gothic), `typewriterSpeed`(文字/秒。0=無効), `letterSpacing`(px 字間), `charAnim`(0=なし 1=ウェーブ 2=ジッター 3=レインボー)+`charAnimAmount/Speed`, `gradientDir`(0=なし 1=横 2=縦)+`gradientColor2` | テキスト。クリックは遮らない。縁取り/影はゲームUIの可読性の要。`typewriterSpeed>0` で Play 中に1文字ずつ表示（UTF-8 単位。`setUiText` で先頭から再生し直し。整列は全文サイズ固定）。`letterSpacing`/`charAnim` は 1 文字ずつ描く per-glyph モード（**wrap とは非両立**=wrap優先）。テキストグラデは本体のみ（縁取り/影には掛からない。金色タイトル等） |
| `UIButton` | `onClickEvent`, `normalColor/hoverColor/pressedColor`, `interactable` | 同一エンティティの `UIImage` を状態色でティント。release-inside でクリック確定。要素が重なった場合は**最前面だけ**が反応（子要素がクリックを吸っても親ボタンへバブリング） |
| `UILayout` | `mode`(0=VBox 1=HBox 2=Grid), `cellW/cellH`(px。VBoxのcellW=0/HBoxのcellH=0は内側いっぱい), `spacing`, `padding`(左上右下px), `gridCols` | **自動レイアウトコンテナ**。直下の子（UIRect持ち）へセル矩形を順番に配る＝手動 offset 計算なしでメニュー列/ツールバー/インベントリ。子はセル内でアンカー解決（全面ストレッチ=セルいっぱい）。スクロールリストは UIScrollView の子コンテンツノードに付ける |

アンカー解決式（`UIRect`、親矩形基準・実ピクセル）:
```
rectMin = parentMin + parentSize * anchorMin + offsetMin
rectMax = parentMin + parentSize * anchorMax + offsetMax
```
アンカー一致（例: 中央固定）なら offset は「中心位置 ± 半サイズ」相当（Inspector 上は位置/サイズ(px)で編集）。
アンカーを引き伸ばす（例: 横ストレッチ）なら offset は左右の余白(px)になる。Inspector の**アンカープリセット**
（9方位＋ストレッチ＋全面）は選択時に見た目の位置を保ったまま anchor/offset を再計算する。

**Lua からの操作**（値の書き換えのみ。ツリー構造はエディタで組む）: `scene:setUiText/getUiText/setUiTypewriter/isUiTypewriterDone/setUiColor/setUiVisible/setUiTexture/setUiFill/getUiFill/setUiRotation/getUiRotation/getUiSlider/setUiSlider/getUiToggle/setUiToggle/getUiScroll/setUiScroll`（§3 Scene 参照）。定番演出は `uifx.*`（§5 prelude 参照）。

**回転/装飾の既知の制限**: ①回転/スキューしたパネルの**中に** `UIScrollView`/`clipChildren` を置くのは非対応（クリップ位置がずれる。逆=スクロールビュー内の回転要素は OK）②`gradientColor2` のアルファは無視される ③回転+角丸+部分 fill の併用はゲージの切り口も丸くなる ④テクスチャ付き形状（shape≠0）/放射 fill の縁は AA なし（縁取りを付けると綺麗）⑤放射 fill と 9-slice は非両立（平板扱い）⑥破線/ブラケット枠は矩形専用・角丸無視。

**ゲームパッド/キーボードのフォーカスナビゲーション**（設定不要で常時有効）:
矢印キー / D-pad / 左スティックでフォーカス移動（位置ベースの空間ナビ）、Enter / Space / A ボタンで決定
（押しで押下表示、離しで確定 = マウスと同じ手触り）。フォーカス中のウィジェットには青いフォーカスリングが出る。
フォーカス中の **UISlider は左右で値変更**（`step` 未設定なら範囲の 1/20 刻み、ホールドでリピート）。
マウスクリックでもフォーカスは移るので併用可。ボタンの `hoverSound` はフォーカス到達時にも鳴る。

**ボタンのクリックを受ける**: 追加 API は無く既存の `events` で受ける。`UIButton.onClickEvent` に設定した名前で、
release-inside 確定の**次フレーム**（`OnUpdate` より前）に `events:emit` 相当で発火する。data は `{source=エンティティID}`（他キー無し）。
```lua
function OnStart(self)
    events:on("start_clicked", function(data)
        -- data.source にボタンのエンティティID(entt raw id)
        goToScene("assets/scenes/game.json")
    end)
end
```

---

## 4. グローバル関数・定数

### ユーティリティ / 永続ストレージ
| 関数 | 説明 |
|---|---|
| `log(...)` | ログ出力（任意個・任意型の引数を `tostring` でタブ区切り連結、`[Lua]` 接頭辞、Info） |
| `logWarn(...)` | 同上（Warn = コンソールで黄色） |
| `logError(...)` | 同上（Error = コンソールで赤） |
| `print(...)` | `log` と同じ経路に出る（素の `print` はどこにも出ないため差し替え済み） |
| `saveNum(key, v)` | シーンをまたいで残る数値ストア（スコア受け渡し等） |
| `loadNum(key, def?)` | 値を取得（無ければ def or 0） |

> `log`/`logWarn`/`logError`/`print` の出力、および Lua の実行時エラー（load/OnStart/OnUpdate）は全てエディタの **コンソールパネル**（アセットブラウザ隣タブ）に集約される。重大度フィルタ（情報/警告/エラー、既定は警告+エラーのみ）・テキスト検索・同一メッセージ折りたたみ・自動スクロール・Play時クリア・新着エラーで自動前面化・行クリックで詳細ペインを備え、下部の入力欄から Lua を1行その場で実行できる（`scene`/`fx`/`camera` 等そのままアクセス可）。

### シーン遷移
| 関数 | 説明 |
|---|---|
| `loadScene(rel)` | シーンを即切替 |
| `nextScene()` | SceneFlow の次シーンへ |
| `quit()` | ゲーム終了 |
| `fadeToScene(rel, dur?=0.6)` | フェード切替 |
| `transitionToScene(rel, type, dur?=0.6)` | トランジション切替（type: 0=Fade,1=Wipe,2=Circle,3=縦Wipe） |

### キーコード定数（`input:isKeyDown(...)` に渡す）
- `KEY_W` `KEY_A` `KEY_S` `KEY_D` `KEY_E` `KEY_Q`
- `KEY_SPACE` `KEY_SHIFT` `KEY_TAB` `KEY_ESCAPE` `KEY_ENTER`
- `KEY_F1` `KEY_F2` `KEY_F3` `KEY_RBUTTON`
- `KEY_UP` `KEY_DOWN` `KEY_LEFT` `KEY_RIGHT`
- `KEY_A`..`KEY_Z`（全英字） / `KEY_0`..`KEY_9`（全数字）

### モーションタイプ定数（`physics:addRigidBody`）
- `MOTION_STATIC`（静的） / `MOTION_KINEMATIC`（運動学的） / `MOTION_DYNAMIC`（動的）

---

## 5. 高レベル prelude ヘルパー

`ScriptEngine` 起動時に自動ロードされる純 Lua API（全スクリプトから利用可）。

### 入力
| 関数 | 説明 |
|---|---|
| `keyDown(name)` | 文字列キー名で押下判定。name: `"W/A/S/D/E/Q/UP/DOWN/LEFT/RIGHT/SPACE/SHIFT/TAB/ENTER/ESC"` |
| `keyPressed(name)` | 同上（押した瞬間） |

### Actor（名前付きエンティティの薄ラッパー）
```lua
local a = actor("Player", { speed=5, solid={"Wall1","Wall2"}, half=0.5 })
```
| メソッド | 説明 |
|---|---|
| `a:entity()` | 対応 Entity（無効なら nil） |
| `a:valid()` | 有効か |
| `a:pos()` | 現在位置（Vec3） |
| `a:setPos(x, y, z)` | 位置設定 |
| `a:moveTopDown(dt, scheme?)` | 見下ろし移動。scheme=`"WASD"`(既定)/`"Arrows"`。solid に当たると軸ごと停止（壁沿いスライド） |
| `a:reached(other, radius?=1.0)` | 相手 Actor に届いたか（XZ 距離） |

### カメラ追従
| 関数 | 説明 |
|---|---|
| `cameraFollow(target, opts)` | 見下ろし追従。opts=`{name="GameCamera",height=13,back=8,pitch=55}` |
| `cameraTPS(target, opts)` | 三人称トレイルカメラ（マウス不要）。opts=`{name,dist,height,pitch,yaw,follow}` |
| `cameraLockOn(playerPos, targetPos, opts, state)` | ロックオンカメラ。戻り=カメラ yaw（度） |

### シーン遷移の別名 / 汎用
| 関数 | 説明 |
|---|---|
| `goToScene(path, dur?)` | `fadeToScene` の別名 |
| `win(dur?)` | `nextScene()` を呼ぶ |
| `clamp(v, lo, hi)` | クランプ |
| `lerp(a, b, t)` | 線形補間 |
| `angleDelta(from, to)` | 角度の最短差（度・-180..180） |

---

## 6. VFX / パーティクル API

### fx（即時放出）
```lua
fx:burst{ x=, y=, z=, count=16, kind="fire", ... }   -- 飛散
fx:ring{ ... }                                        -- 放射状リング
fx:beam{ x0=,y0=,z0=, x1=,y1=,z1=, width=, kind= }    -- 連続ビーム/火柱/稲妻
fx:pulse(amt?=0.5)                                     -- 画面パルス（クロマ+放射ブラー）
fx:clear()                                             -- 全消去
```

**burst / ring のテーブルキー**:
`x,y,z`（位置）/ `count`（個数）/ `dx,dy,dz`（方向）/ `spread`（拡散）/ `speed,speedVar` / `size,sizeEnd` / `life,lifeVar` /
`r,g,b`（開始色）/ `rEnd,gEnd,bEnd`（終了色）/ `rMid,gMid,bMid`（中間色）/ `intensity`（HDR・>1 でブルーム）/
`gravity` / `drag` / `up`（上向きバイアス）/ `stretch`（速度方向ストレッチ）/ `turbStrength,turbFreq`（カール乱流）/
`flicker,flickerFreq`（明滅）/ `blend`（0=加算,1=α前乗算）/ `kind`

**kind**（文字列 or 数値）: `glow`(0) / `fire`(1) / `smoke`(2) / `spark`(3) / `magic`(4) / `electric`=`lightning`(5) / `ring`=`shockwave`(6) / `star`=`flare`(7)
**beam の kind**: `energy`(0) / `electric`=`lightning`(1) / `fire`(2)

### FX.*（派手プリセット）
| 関数 | 説明 |
|---|---|
| `FX.explosion(x,y,z, scale?, r?,g?,b?)` | 爆発（フラッシュ+火球+火花+リング+煙） |
| `FX.shockwave(x,y,z, count?, speed?, r?,g?,b?)` | 衝撃波リング+放射光筋 |
| `FX.spark(x,y,z, count?, r?,g?,b?)` | 着弾火花 |
| `FX.trail(x,y,z, r?,g?,b?)` | 軌跡/オーラ点（毎フレーム呼ぶ） |
| `FX.supernova(x,y,z, scale?)` | レベルアップ超新星 |
| `FX.hit(amount?)` | ヒット時の画面パルス |
| `FX.beam(x0,y0,z0, x1,y1,z1, r?,g?,b?, width?, kind?, intensity?)` | 連続ビーム（毎フレーム） |
| `FX.lightning(x0,y0,z0, x1,y1,z1, r?,g?,b?, width?)` | 稲妻ビーム |
| `FX.pillar(x,y,z, height?, radius?, r?,g?,b?)` | 動的火柱 |

### vfx（統一窓口）
```lua
vfx.register(name, fn)            -- コードプリセット登録
vfx.play(name, x, y, z, scale?)   -- Effekseer 実体があれば優先、無ければコード
```
既定登録済み: `"explosion"` / `"supernova"` / `"spark"` / `"hit"`

### uifx.*（ゲーム内UIの定番演出ワンライナー）
`e` は Entity かエンティティID（ボタンイベントの `e.source` そのまま）。実体は `scene:tweenUi` の組み合わせ。

| 関数 | 説明 |
|---|---|
| `uifx.punch(e, s?, dur?)` | ボタンを押した感（一瞬膨らんで戻る。既定 1.15倍/0.22秒） |
| `uifx.flash(e, r?, g?, b?, dur?)` | 色フラッシュ（既定=白く光る。ダメージ赤は `uifx.flash(e, 3, 0.3, 0.3)`） |
| `uifx.shake(e, amp?, dur?)` | 振動（既定 10px/0.4秒。減衰付き） |
| `uifx.hit(e, amp?)` | 赤フラッシュ + 振動（被ダメの定番セット） |
| `uifx.bounceIn(e, dur?)` | ぽよんと登場（0 からバウンドで等倍へ） |
| `uifx.flipIn(e, dur?)` | ぺしゃんこ→開く（フリップ風。結果表示/カード公開） |
| `uifx.popOut(e, dur?)` | 縮んで消える（ポップアップを閉じる） |
| `uifx.fadeIn(e, dur?)` / `uifx.fadeOut(e, dur?)` | フェード（fadeOut 後もクリックは残る。消し切るなら `scene:hideUi`） |
| `uifx.stagger(list, step?, fn, ...)` | リスト項目の順次入場（間隔の相場 0.05〜0.10秒）。`fn` に下の slideIn 系/popIn を渡す |
| `uifx.slideInLeft/Right/Up(e, delay?, dist?, dur?)` | 方向スライド入場（`expo`。delay 付き=stagger と組む） |
| `uifx.popIn(e, delay?, dur?)` | ぽんと出る（delay 対応版 bounceIn） |
| `uifx.countTo(e, to, dur?, fmt?)` | 数字ロール（スコア/所持金。`fmt` 例 `"%06d"`） |
| `uifx.fillTo(e, v, dur?, easing?)` | ゲージをなめらかに増減（fillAmount 絶対値 0..1） |
| `uifx.damageBar(front, ghost, v, ghostDelay?)` | ゴーストバー付きダメージ（本体即落ち+背後バーが遅れて追従=遅延削れ） |
| `uifx.wiggle(e, deg?, dur?)` | 注目のゆらぎ（通知バッジを左右にクイックに振る） |
| `uifx.heartbeat(e, s?, dur?)` | ドクドク2連パルス（クールダウン完了/低HP警告） |

```lua
events:on("startClicked", function(e)
  uifx.punch(e.source)   -- クリックの気持ちよさはこれ1行
end)
```

---

## 7. ECS コンポーネント全一覧

（`src/ecs/Components.h`。Inspector / SceneSerializer / MCP `set_component` で扱える）

### 基本
| コンポーネント | フィールド（既定値） |
|---|---|
| `NameTag` | `name` |
| `Tag` | `tags`（string 配列。`scene:queryByTag` で列挙） |
| `DataComponent` | `values`（動的 key→DataValue。Lua から読み書き） |
| `Transform` | `position`,`rotation`(Euler度),`scale`,`quaternion`,`useQuaternion`,`parent`(親エンティティ) |
| `MeshRenderer` | `modelPath`, `overrideMetallic=-1`, `overrideRoughness=-1`, `uvScaleU=1`, `uvScaleV=1` |
| `GridPlane` | `enabled=true`（エディタグリッド床） |

### ライト
| コンポーネント | フィールド（既定値） |
|---|---|
| `PointLight` | `color=(1,1,1)`, `intensity=1`, `range=10`, `castShadows=false`（同時最大2灯、カメラ近い順） |
| `DirectionalLight` | `direction=(0,-1,0)`, `color=(1,1,1)`, `intensity=1`, `ambient=0.25`（CSM4分割で常時影あり） |
| `SpotLight` | `color`, `intensity=3`, `range=15`, `direction=(0,-1,0)`, `innerConeDeg=18`, `outerConeDeg=28`, `castShadows=false`（同時最大4灯、カメラ近い順） |

### カメラ / 2D / 音
| コンポーネント | フィールド（既定値） |
|---|---|
| `CameraComponent` | `fovDegrees=60`, `nearClip=0.1`, `farClip=1000`, `isActive=false`, `projection`(Perspective/Orthographic), `orthoSize=10` |
| `Sprite2D` | `texturePath`, `layer=0`, `size=(1,1)`, `uvMin`,`uvMax`, `color=(1,1,1,1)`, `worldSpace=true`, `billboard=false` |
| `AudioSource` | `clipPath`, `volume=1`, `loop=false`, `spatial=true`, `playOnStart=true`, `minDistance=1`, `maxDistance=30` |

### アニメーション
| コンポーネント | 説明 |
|---|---|
| `SkeletalAnimation` | スケルタル（skeleton/animator/clips）。`Entity:playAnim*` で再生 |
| `NodeAnimationComp` | ノードアニメーション |

### 物理
| コンポーネント | フィールド（既定値） |
|---|---|
| `RigidBody` | `motionType=Dynamic`, `mass=1`, `restitution=0.4`, `friction=0.3`, `linearDamping=0.02`, `angularDamping=0.01`, `useGravity=true` |
| `BoxCollider` | `halfExtents=(0.5,0.5,0.5)`, `offset` |
| `SphereCollider` | `radius=0.5`, `offset` |
| `CapsuleCollider` | `radius=0.5`, `halfHeight=1`, `offset` |
| `ConvexHullCollider` | `points`(頂点), `offset` |
| `CharacterController` | `radius=0.4`, `halfHeight=0.6`, `offset`, `mass=70`, `maxSlopeDeg=50`, `stepHeight=0.3`, `jumpSpeed=6`, `gravityScale=1`（RigidBody と排他） |

### スクリプト / ゲームロジック
| コンポーネント | フィールド |
|---|---|
| `LuaScript` | `scriptPath`, `enabled=true`, `props`（公開プロパティのインスタンス値） |
| `Gimmick` | `kind`(0=StaticWall/1=SpikePulse/2=SlideX/3=SlideZ), `period=4`, `phase`, `amplitude=1.6`, `threshold=0.5`, `solid=true`, `deadly=false` |
| `ParticleEmitter` | §8 参照（配置できるエフェクト部品） |
| `Trigger` | §8 参照（範囲イベント部品） |

---

## 8. データ駆動オーサリング

エディタで配置・JSON で記述できる「コードを書かないゲームロジック」。`--validate` で参照グラフを検証可能。

### ParticleEmitter（配置エフェクト）
| フィールド（既定値） | 説明 |
|---|---|
| `kind=0` | 見た目: 0=Glow,1=Fire,2=Smoke,3=Spark,4=Magic,5=Electric,6=Ring,7=Star |
| `blend=0` | 0=加算 / 1=α前乗算（煙） |
| `rate=30` | 連続放出レート（個/秒。0 で連続なし） |
| `playOnStart=true` / `looping=true` / `duration=1` | 再生制御（looping=false で duration 秒のワンショット） |
| `dir=(0,1,0)`,`spread=0.4`,`speed=3`,`speedVar=0.4` | 放出方向・速度 |
| `size=0.3`,`sizeEnd=0`,`life=0.8`,`lifeVar=0.3` | サイズ・寿命 |
| `color=(1,0.6,0.2)`,`colorEnd=(1,0.12,0.05)` | 開始/終了色 |
| `intensity=3`,`gravity=0`,`drag=1`,`up=0`,`stretch=0` | HDR/物理/ストレッチ |

Trigger の `PlayEffect` / `StopEffect` で発火・停止できる。

### Trigger（範囲イベント）
| フィールド（既定値） | 説明 |
|---|---|
| `shape=0` | 0=Box,1=Sphere |
| `halfExtents=(1,1,1)` | Box サイズ（Transform.scale 乗算） |
| `radius=1` | Sphere 半径 |
| `offset` | 判定中心ローカルオフセット |
| `filter`（空=Player） | 反応する対象エンティティ名 |
| `once=false` | 一度発火で無効化 |
| `actions` | TriggerAction の配列 |

### TriggerAction
```jsonc
{ "when": 0, "type": 4, "target": "WinBurst", "str": "", "num": 0, "vec": [0,0,0] }
```
- **when**: 0=Enter / 1=Exit / 2=Stay
- **type**（アクション種類）:

| # | type | 動作 |
|---|---|---|
| 0 | `Enable` | target の LuaScript を有効化 |
| 1 | `Disable` | target の LuaScript を無効化 |
| 2 | `Destroy` | target を削除 |
| 3 | `Move` | target を vec だけ相対移動 |
| 4 | `PlayEffect` | target の ParticleEmitter を放出開始 |
| 5 | `StopEffect` | target の ParticleEmitter を停止 |
| 6 | `PlaySound` | target の AudioSource を再生 |
| 7 | `LoadScene` | str のシーンへ即切替 |
| 8 | `FadeToScene` | str のシーンへフェード切替（num=秒） |
| 9 | `SetProperty` | target の実行中スクリプトの `self[str] = num` |
| 10 | `EmitEvent` | イベントバスへ `events:emit(str, {value=num, target=...})` |

### ScriptProp（プロパティ）型
`Float`(0) / `Int`(1) / `Bool`(2) / `String`(3) / `Vec3`(4) / `Color`(5) / `Entity`(6)

---

## 9. コマンドラインフラグ

`DX12Engine.exe [フラグ]`（`src/main.cpp`）

| フラグ | 動作 |
|---|---|
| （なし） | 配布レイアウト（exe 隣に `game.json`）ならゲームモード、無ければエディタ |
| `--game` | ゲームモードで起動 |
| `--editor` | 強制エディタ起動 |
| `--build [<projectDir>]` | ヘッドレスでゲームをビルド（exe+DLL+assets を `build/game/` へ出力） |
| `--validate <scene.json>` | ヘッドレスでシーンの参照グラフを検証（参照切れ・スクリプト不在を報告）。終了コード 0=PASS / 1=FAIL、`validate_report.txt` 出力 |
| `--updated` | 自動アップデート適用後の再起動マーカー（内部用） |

---

## 10. MCP（AI ブリッジ）ツール一覧

起動中のエディタを Claude Code / Codex から操作する MCP サーバ（`tools/mcp-server`）。ツール名は `dx12_` 接頭辞。
詳細・遅延同期・error_code は [`MCP.md`](MCP.md) を参照。

### 読み取り系（同期）
| ツール | 説明 |
|---|---|
| `dx12_ping` | 接続確認（mode/entityCount/sceneGeneration） |
| `dx12_list_entities` | エンティティ一覧（name_prefix/component_type フィルタ） |
| `dx12_get_entity` | 1 体の全コンポーネント値 |
| `dx12_find_entity` | 名前で検索 |
| `dx12_query_entities` | tag / box でクエリ |
| `dx12_list_scenes` | シーン一覧 |
| `dx12_list_assets` | アセット一覧（model/texture/script/audio/scene/prefab） |
| `dx12_get_mode` | Editor / Playing |
| `dx12_get_log` | ログ末尾 N 行 |
| `dx12_describe_components` | コンポーネント定義（jsonKey/fields/default） |
| `dx12_get_scene_settings` | スカイボックス/IBL 設定 |
| `dx12_screenshot` | スクショ（PNG） |

### 編集系（同期）
| ツール | 説明 |
|---|---|
| `dx12_set_transform` | 位置/回転/quaternion/スケール |
| `dx12_set_component` | コンポーネント設定（jsonKey + data） |
| `dx12_remove_component` | コンポーネント削除 |
| `dx12_set_parent` | 親子付け（省略で解除） |
| `dx12_rename_entity` | リネーム |
| `dx12_select_entity` | 選択 |
| `dx12_focus_camera` | カメラフォーカス |
| `dx12_set_pbr` | metallic/roughness/UV スケール |
| `dx12_set_scene_settings` | スカイボックス/IBL 設定 |
| `dx12_undo` / `dx12_redo` | Undo/Redo |
| `dx12_save_scene` | シーン保存 |
| `dx12_create_lua_component` | Lua コンポーネント生成（構文検証つき） |
| `dx12_attach_lua_component` | Lua をエンティティに添付 |

### 生成・削除・モード遷移（遅延同期）
| ツール | 説明 |
|---|---|
| `dx12_create_entity` | box/sphere/plane/empty を生成 |
| `dx12_spawn_model` | モデル（.gltf/.glb/.fbx/.obj）を生成 |
| `dx12_spawn_prefab` | プレハブ（.prefab）を生成 |
| `dx12_duplicate_entity` | 複製 |
| `dx12_delete_entity` | 削除 |
| `dx12_open_scene` / `dx12_new_scene` | シーン操作 |
| `dx12_play` / `dx12_stop` | Play / Stop |

### Node 合成ツール
| ツール | 説明 |
|---|---|
| `dx12_batch` | 複数 op を順次実行（往復削減） |
| `dx12_focus_and_screenshot` | フォーカス→描画→スクショ |

---

## 11. C++ 主要モジュール API

エンジンを C++ から埋め込む / 拡張する場合の **主要クラスの public API**（よく使うものを抜粋）。全クラスは `namespace dx12e`。
※ここに無い graphics 低レベル層（`GraphicsDevice` / `SwapChain` / `DescriptorHeap` / `PipelineState` / `RootSignature` / `Buffer` / `Texture` / `FrameResources` / `CommandQueue` 等）、エディタ（`EditorLayer` / 各 Panel / `UndoSystem`）、VFS/pak、Project 系は内部実装寄りのため未収載。必要なら追記する。

### Application（`core/Application.h`）— アプリ基盤・メインループ
| メソッド | 説明 |
|---|---|
| `Initialize(hInstance, nCmdShow, gameMode=false, ...)` | ウィンドウ生成・全サブシステム初期化 |
| `Run()` | メインループ開始（戻ってこない） |
| `Shutdown()` | 全リソース解放（GPU メンバを明示 reset） |
| `BuildGameStandalone()` | ヘッドレスでゲームをビルド（`--build` の実体） |

> 描画・Play/Stop・プロジェクト読込・Git 等は private なオーケストレーション。埋め込みは基本 `Initialize → Run → Shutdown` のみ。

### Scene（`scene/Scene.h`）— エンティティ管理
| メソッド | 説明 |
|---|---|
| `Initialize(resourceManager, device, srvHeap, cmdList)` | 初期化 |
| `Spawn(name, modelPath, pos, rot={0,0,0}, scale={1,1,1})` → Entity | モデル生成 |
| `SpawnPlane(name, pos, size=50, gridShader=false)` → Entity | 平面 |
| `SpawnBox(name, pos, rot, scale)` → Entity | ボックス |
| `SpawnSphere(name, pos, radius=0.5)` → Entity | 球 |
| `Remove(entity)` / `Clear()` | 削除 / 全消去 |
| `Update(dt)` | 更新 |
| `ForEachEntity(fn)` | NameTag+Transform を持つ全エンティティを巡回（テンプレート） |
| `FindEntity(name)` → Entity | 名前検索 |
| `GetEntityCount()` → size_t | 総数 |
| `QueryByTag(tag)` → vector\<entity\> | タグでクエリ |
| `QueryInBox(minX, minZ, maxX, maxZ, tag="")` → vector\<entity\> | XZ 矩形クエリ |
| `GetDevice()` / `GetRegistry()` | デバイス / entt レジストリ参照 |
| `GetPostSettings()` / `GetSkyboxSettings()` / `GetSSAOSettings()` | シーン単位の描画設定（読み書き） |

`SkyboxSettings`: `envMapPath`(.dds), `iblIntensity=1`, `skyboxIntensity=1`, `drawSkybox=true`

### Entity（`scene/Entity.h`）— ECS ハンドルの薄ラッパー
| メソッド | 説明 |
|---|---|
| `AddComponent<T>(args...)` → T& | コンポーネント追加 |
| `GetComponent<T>()` → T& | 取得 |
| `HasComponent<T>()` → bool | 有無 |
| `RemoveComponent<T>()` | 削除 |
| `GetHandle()` → entt::entity / `IsValid()` → bool | ハンドル / 有効性 |
| `operator== / !=` | 比較 |

### Camera（`renderer/Camera.h`）— FPS / 正射カメラ
| メソッド | 説明 |
|---|---|
| `SetPerspective(fovYRad, aspect, nearZ, farZ)` | 透視投影設定 |
| `SetOrthographic(viewHeight, aspect, nearZ, farZ)` | 正射投影設定 |
| `SetAspect(aspect)` / `IsOrthographic()` | アスペクト / 正射判定 |
| `LookAt(eye, target, up={0,1,0})` | 注視（固定カメラ） |
| `MoveForward/MoveRight/MoveUp(dist)` / `Rotate(yawΔ, pitchΔ)` | 移動 / 回転 |
| `GetViewMatrix() / GetProjectionMatrix() / GetViewProjMatrix()` | 行列取得 |
| `GetPosition()/SetPosition()` `GetForward()` `GetYaw()/GetPitch()` `SetYaw()/SetPitch()` | 状態 |
| `GetFovY()/GetAspect()/GetNearZ()/GetFarZ()` | CSM カスケード用ゲッター |
| `Get/SetMoveSpeed()` `Get/SetMouseSensitivity()` | 速度 / 感度 |

### Mesh（`renderer/Mesh.h`）— ジオメトリ
| メソッド | 説明 |
|---|---|
| `Initialize(device, vertices, indices)` | 任意メッシュ生成 |
| `InitializeAsBox/AsPlane/AsSphere(device, ...)` | プリミティブ生成 |
| `FinishUpload()` | アップロード完了処理 |
| `GetVertexBuffer()/GetIndexBuffer()/GetIndexCount()` | バッファ |
| `SetMaterial(mat)/GetMaterial()/GetMaterialMutable()` | マテリアル |
| `GetAABBMin()/GetAABBMax()` | バウンディング |
| `GetPositions()` / `GetVertexColor()` | 頂点座標 / 代表色 |
| `ApplyUVScale(device, u, v)` | UV タイリング（VB 再作成・GPU 同期） |
| `SetVertexColor(device, r, g, b, a=1)` | 頂点カラー一括（生成時に1回） |
| `static GetInputLayout()/GetInputLayoutCount()` | 入力レイアウト |

`Vertex`: `position` / `normal` / `color` / `texCoord` / `tangent` / `boneIndices` / `boneWeights`

### Material（`renderer/Material.h`）— PBR マテリアル（struct）
`albedoTexture` / `normalMapTexture` / `metalRoughnessTexture`（R=未使用,G=roughness,B=metallic）/ `defaultMetallic=1` / `defaultRoughness=1` / `srvBlockIndex`

### PhysicsSystem（`physics/PhysicsSystem.h`）— Jolt 物理
Lua の `physics:*`（§3）の C++ 実体。加えて以下を持つ:
| メソッド | 説明 |
|---|---|
| `Initialize()` / `Update(dt, registry)` / `Shutdown()` | ライフサイクル |
| `RegisterBody / UnregisterBody / UnregisterAllBodies(registry, entity)` | 剛体の登録/解除 |
| `RegisterCharacter / UnregisterCharacter / UnregisterAllCharacters` | CharacterVirtual の登録/解除 |
| `StepCharacters(fixedDt, registry)` / `SyncCharactersToTransforms(registry)` | キャラ更新 |
| `ApplyForce / ApplyImpulse / SetLinearVelocity / GetLinearVelocity / SetPosition(bodyId, ...)` | 物理操作（bodyId 指定） |
| `Raycast(origin, dir, maxDist=1000)` → RaycastHit | レイキャスト |
| `OverlapBox / OverlapSphere(center, ..., out, cap)` → size_t | 空間クエリ（バッファ書き込み） |
| `SetEventBus(bus)` | 接触イベントの配信先（`engine.contact.enter/exit`） |
| `SetPaused(b)` / `Step(dt)` / `SetGravity(g)` | 時間モデル |
| `IsInitialized()` / `ResetAccumulator()` | 状態 |

`RaycastHit`(C++): `hit` / `distance` / `bodyId` / `point` / `normal`

### AudioSystem（`audio/AudioSystem.h`）— XAudio2 / X3DAudio
Lua の `audio:*`（§3）の C++ 実体。加えて:
| メソッド | 説明 |
|---|---|
| `Initialize(assetsDir)` / `Shutdown()` / `SetAssetsDir(dir)` | ライフサイクル |
| `SetListener(px,py,pz, fx,fy,fz, ux,uy,uz)` | 3D リスナー設定（通常カメラ） |
| `PlaySFXSpatial(path, x,y,z, minD, maxD, vol=1, loop=false)` → slotId | 空間 SFX |
| `UpdateSpatialEmitter(slotId, x,y,z)` | エミッタ追従 |
| `Update()` | 毎フレーム定位再計算 |

### ParticleSystem（`renderer/ParticleSystem.h`）— CPU sim + GPU インスタンシング
Lua の `fx:*`（§6）の C++ 実体。
| メソッド | 説明 |
|---|---|
| `Initialize(device, rtvFormat, dsvFormat, shaderDir)` | 初期化 |
| `Emit(EmitParams)` / `EmitBeam(BeamParams)` | 放出 |
| `Update(dt)` / `Clear()` | 更新 / 全消去 |
| `Render(cmd, viewProj, camRight, camUp, camPos)` | 描画（scene パス内・HDR RT バインド済み） |
| `SetSceneDepth(srv, projA, projB, invRTW, invRTH)` / `DisableSceneDepth()` | ソフトパーティクル深度 |
| `SetTime(t)` / `AddPulse(amount)` / `GetPulse()` / `AliveCount()` | 状態 |

enum `ParticleKind`: Glow/Fire/Smoke/Spark/Magic/Electric/Ring/Star ／ `ParticleBlend`: Additive/AlphaPremul ／ `BeamKind`: Energy/Electric/Fire
`EmitParams` / `BeamParams` のフィールドは §6 の fx テーブルキーと対応。

### InputSystem（`input/InputSystem.h`）— Raw Input + XInput
| メソッド | 説明 |
|---|---|
| `Initialize(hwnd)` / `Update(dt)` | 初期化 / フレーム開始時（dt はパッド振動タイマー消費用。既定 0） |
| `IsKeyDown/IsKeyPressed/IsAsyncKeyDown(vk)` | キー判定 |
| `GetMouseDeltaX/Y()` `IsMouseCaptured()` `IsRightMouseDown()` | マウス |
| `SetMouseCapture(b)` / `ToggleMouseCapture()` | キャプチャ |
| `OnKeyDown/OnKeyUp/OnRawInput/OnMouseButton/OnFocusLost` | WndProc から呼ぶ |
| `IsPadConnected(pad)` / `GetConnectedPadCount()` | XInput 接続状態（pad=0..3、`kMaxGamepads`=4） |
| `IsPadButtonDown/Pressed/Released(pad, button)` | ボタン判定（`button` は `XINPUT_GAMEPAD_*` ビット値） |
| `GetPadLeftStickX/Y / GetPadRightStickX/Y(pad)` | スティック（`GamepadState` 内で円形デッドゾーン適用済み、-1..1） |
| `GetPadLeftTrigger/RightTrigger(pad)` | トリガー（デッドゾーン適用済み、0..1） |
| `SetPadVibration(pad, left, right)` | 振動（`XInputSetState` ラップ。0.0..1.0） |
| `SetPadVibrationTimed(pad, left, right, sec)` | sec 秒後に `Update()` 内で自動停止する振動 |

`Update()` 内 `UpdateGamepads(dt)` が毎フレーム4台分 `XInputGetState` をポーリングし、`GamepadState`（`buttons`/`prevButtons` でエッジ判定、`vibrationTimeLeft` でタイマー振動を管理）を更新する。`xinput.lib` は `src/input/CMakeLists.txt` で `PRIVATE` リンク。

### ScriptEngine（`scripting/ScriptEngine.h`）— Lua ランタイム
| メソッド | 説明 |
|---|---|
| `Initialize(scene, input, camera, audio, physics, assetsDir)` | 初期化（バインド登録＋prelude） |
| `SetAssetsDir / SetParticleSystem / SetEventBus(...)` | 依存注入 |
| `LoadScript(path)` / `LoadScriptFromString(code, name)` | スクリプト読込 |
| `SetScreenSize(w, h)` | `SCREEN_W/H` 公開 |
| `CallOnStart()` / `CallOnUpdate(dt)` | グローバル `game.lua` ライフサイクル |
| `AttachScriptToEntity(e, path)` / `DetachScriptFromEntity(e)` | アタッチ |
| `GetPropertySchema(path)` / `InvalidatePropertySchema(path)` | `properties` 解析 |
| `OnPlayStart()` / `OnPlayStop()` / `UpdateAttachedScripts(dt)` / `UpdateTriggers(dt)` | Play ライフサイクル |
| `ReloadScript(e)` | ホットリロード |
| `CheckLuaSyntax(code, err)` → bool | 構文チェック（実行しない・MCP create 用） |
| `GetLastError()` / `ClearError()` | エラー |
| `SetLoadScene/NextScene/Quit/Transition/Ui*Callback(...)` | ゲーム制御コールバック注入（Application が結線） |

`ScriptPropDef`: `name` / `type` / `def`(既定値) / `hasRange` / `minVal` / `maxVal` / `label`

### ResourceManager（`resource/ResourceManager.h`）— アセットキャッシュ
| メソッド | 説明 |
|---|---|
| `Initialize(device, srvHeap, cmdList)` | 初期化 |
| `GetOrLoadTexture(path, cmdList, srgb=true)` → Texture* | テクスチャ（キャッシュ） |
| `GetOrLoadModel(path, cmdList)` → CachedModel* | モデル（キャッシュ） |
| `GetOrLoadEmbeddedTexture(key, data, size, hint, cmdList)` | 埋め込みテクスチャ |
| `GetDefaultWhite/Normal/MetalRoughnessTexture()` | デフォルトテクスチャ |
| `FinishUploads()` | アップロード完了 |

### ModelLoader（`resource/ModelLoader.h`）— Assimp ローダー（static）
| メソッド | 説明 |
|---|---|
| `static LoadFromFile(device, cmdList, path, resourceManager)` → ModelData | glTF/OBJ/FBX 読込（メッシュ/マテリアル/スケルトン/アニメ） |
| `static LoadAnimationsFromFile(path, skeleton)` → clips | アニメだけ追加読込 |

### SceneSerializer（`scene/SceneSerializer.h`）— シーン JSON（static）
| メソッド | 説明 |
|---|---|
| `Save / Load(scene, path, assetsDir)` | シーン保存 / 読込 |
| `ApplyOverrides(scene, path, assetsDir)` | Transform/Material のみ上書き |
| `SaveToString / LoadFromString(...)` | インメモリ（Play→Stop 復元） |
| `SerializeEntity / InstantiateEntity / DuplicateEntity(...)` | 単一エンティティ（複製/クリップボード） |
| `SerializeSubtree / InstantiateSubtree(...)` | サブツリー（親子込み） |
| `SavePrefab / InstantiatePrefab(...)` | プレハブ .prefab |

### SceneFlow（`scene/SceneFlow.h`）— シーン遷移グラフ
`assets/sceneflow.json` と対応（`start` + 各シーンの `next`/`onFail`）。
| メソッド | 説明 |
|---|---|
| `Load(path) / LoadFromString(json) / Save(path)` / `IsLoaded()` | I/O |
| `Start() / SetStart(s)` | 開始シーン |
| `Next(currentRel) / OnFail(currentRel)` | 次 / 失敗時の遷移先 |
| `SetNext(scene, next) / SetOnFail(scene, onFail)` / `Flow()` | 編集 / 全体取得 |

---

*このドキュメントは `src/scripting/ScriptEngine.cpp` / `src/ecs/Components.h` / `src/main.cpp` / `docs/MCP.md` および各モジュールの公開ヘッダ（`src/**/*.h`）から抽出。API を追加・変更したら本ファイルも更新すること。*
