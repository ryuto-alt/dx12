---@meta
-- ============================================================
--  DX12 Engine — Lua API 型定義（LuaLS / EmmyLua アノテーション）
--  VSCode の Lua 拡張（sumneko.lua）がこれを読むと、エンジン API の
--  補完・ホバードキュメント・引数ヒント・型チェックが効くようになる。
--
--  ◆ 正（source of truth）: src/scripting/ScriptEngine.cpp
--      RegisterBindings / RegisterPhysicsBindings / RegisterEventsBinding
--  ◆ prelude 由来の高レベルヘルパー（actor / cameraFollow / FX / vfx …）は
--      dx12e-prelude.lua にある（正: LoadPrelude の kPrelude 文字列）
--  ◆ エンジン側で Lua API を追加・変更したら必ずこのファイルも更新すること
--
--  ※ このファイルは実行されない（---@meta 宣言済みの定義専用ファイル）
-- ============================================================

-- ============================================================
-- 型: Vec3 / Transform / Entity
-- ============================================================

---3次元ベクトル（DirectX::XMFLOAT3 のラッパー）
---@class Vec3
---@field x number
---@field y number
---@field z number
Vec3 = {}

---Vec3 を生成する
---```lua
---local v = Vec3.new(1, 2, 3)
---tf.position = Vec3.new(x, y, z)  -- 位置は Vec3 丸ごと代入が安全
---```
---@param x? number
---@param y? number
---@param z? number
---@return Vec3
function Vec3.new(x, y, z) end

---エンティティの変換情報
---@class Transform
---@field position Vec3 位置
---@field rotation Vec3 回転（オイラー角・度）
---@field scale Vec3 拡縮

---`e:hasComponent(...)` に渡せるコンポーネント型名
---@alias ComponentName
---| "Transform"
---| "NameTag"
---| "Tag"
---| "DataComponent"
---| "MeshRenderer"
---| "SkeletalAnimation"
---| "NodeAnimation"
---| "GridPlane"
---| "PointLight"
---| "DirectionalLight"
---| "SpotLight"
---| "Camera"
---| "Sprite2D"
---| "AudioSource"
---| "Gimmick"
---| "RigidBody"
---| "BoxCollider"
---| "SphereCollider"
---| "CapsuleCollider"
---| "ConvexHullCollider"
---| "CharacterController"
---| "LuaScript"
---| "ParticleEmitter"
---| "TrailRenderer"
---| "DecalComponent"
---| "Trigger"
---| "NetworkIdentity"
---| "NetworkTransform"
---| "UICanvas"
---| "UIRect"
---| "UIImage"
---| "UIText"
---| "UIButton"
---| "UISlider"
---| "UIToggle"
---| "UIScrollView"
---| "UILayout"
---| "UIAnimator"
---| "UIAnimPlayer"
---| "SpriteAnimator"
---| "PrefabLink"

---シーン内のエンティティ
---@class Entity
---@field name string エンティティ名（読み取り専用）
---@field transform Transform Transform（読み書き可）
local Entity = {}

---有効なエンティティか（削除済み/未発見なら false）
---@return boolean
function Entity:isValid() end

---コンポーネントを持っているか。未知の型名は警告ログが出る
---@param type ComponentName
---@return boolean
function Entity:hasComponent(type) end

---スケルタルアニメをクリップ番号で再生（クロスフェード）
---@param clipIndex integer 0始まり
---@param blendDuration number クロスフェード秒
function Entity:playAnim(clipIndex, blendDuration) end

---スケルタルアニメをクリップ名で再生（クロスフェード）
---@param name string クリップ名
---@param blendDuration number クロスフェード秒
function Entity:playAnimByName(name, blendDuration) end

---アニメのループ ON/OFF
---@param loop boolean
function Entity:setLooping(loop) end

---スケルタルアニメの再生速度倍率（既定 1.0、2.0 で 2 倍速、0 で一時停止）。
---移動速度と足の同期に使う。
---@param speed number
function Entity:setAnimSpeed(speed) end

---アニメクリップ数
---@return integer
function Entity:getAnimCount() end

---アニメクリップ名を取得
---@param index integer 0始まり
---@return string
function Entity:getAnimName(index) end

--=============================================================================
-- アニメーションステートマシン（.animfsm / AnimatorController）
-- グラフの構造はアセット側。Lua が触るのはパラメータだけ。
-- AnimatorController が無いときは黙って no-op / 既定値を返す。
--=============================================================================

---FSM の float パラメータを書く（例 setAnimFloat("speed", 3.2)）
---@param name string
---@param value number
function Entity:setAnimFloat(name, value) end

---FSM の bool パラメータを書く
---@param name string
---@param value boolean
function Entity:setAnimBool(name, value) end

---FSM のトリガを立てる（1 回だけ。条件を満たした遷移が消費する）
---@param name string
function Entity:setAnimTrigger(name) end

---FSM の float パラメータを読む（無ければ 0）
---@param name string
---@return number
function Entity:getAnimFloat(name) end

---FSM の bool パラメータを読む（無ければ false）
---@param name string
---@return boolean
function Entity:getAnimBool(name) end

---現在のステート名（無ければ ""）
---@param layer integer? 省略で 0
---@return string
function Entity:getAnimStateName(layer) end

---現ステートの正規化時間 0..1（攻撃の当たり判定窓などに使う）
---@param layer integer? 省略で 0
---@return number
function Entity:getAnimNormalizedTime(layer) end

---ステートへ強制遷移（デバッグ / カットシーン用）
---@param stateName string
---@param blendDuration number? 省略で 0.2
function Entity:playAnimState(stateName, blendDuration) end

---レイヤー重み 0..1（上半身レイヤーのフェードイン等）
---@param layer integer
---@param w number
function Entity:setAnimLayerWeight(layer, w) end

---レイヤー重みを読む
---@param layer integer
---@return number
function Entity:getAnimLayerWeight(layer) end

--=============================================================================
-- フット IK（接地補正）。FootIK コンポーネントが要る。Play 中のみ動く。
--=============================================================================

---フット IK の効きを実行時に変える 0..1
---@param w number
function Entity:setFootIKWeight(w) end

---フット IK の効きを読む
---@return number
function Entity:getFootIKWeight() end

---フット IK のレイが地面に当たっているか
---@param rightFoot boolean? true で右足、省略/false で左足
---@return boolean
function Entity:isFootGrounded(rightFoot) end

---UIアニメクリップ(.uianim)を頭から再生する。UIAnimPlayer が無ければ自動で付く。
---clipPath 省略時は既に割り当てられているクリップを再生。
---@param clipPath string? assets 相対（例 "uianim/menu_open.uianim"）
function Entity:playUiAnim(clipPath) end

---UIアニメの再生を止める（姿勢はその場で固定）
function Entity:stopUiAnim() end

---UIアニメを任意時刻へシークする。再生中かどうかは変えない
---@param t number 秒（0..クリップの尺）
function Entity:setUiAnimTime(t) end

---UIアニメの再生速度。負の値で逆再生、0 で一時停止
---@param speed number
function Entity:setUiAnimSpeed(speed) end

---スプライトシート(.spranim)のシーケンスを頭から再生する。
---SpriteAnimator が無ければ自動で付く。同じ名前を再指定しても頭から再生し直す。
---@param seqName string シーケンス名（例 "run"）
function Entity:playSprite(seqName) end

---スプライト連番アニメの再生を止める
function Entity:stopSprite() end

---スプライトシートを差し替える（再生位置は 0 に戻る）
---@param sheetPath string assets 相対（例 "spriteanim/player.spranim"）
function Entity:setSpriteSheet(sheetPath) end

-- ============================================================
-- scene: エンティティの取得・生成・検索
-- ============================================================

---`scene:gimmicks()` が返す要素
---@class GimmickInfo
---@field e Entity
---@field name string
---@field kind string
---@field period number
---@field phase number
---@field amplitude number
---@field threshold number
---@field solid boolean
---@field deadly boolean

---@class Scene
local Scene = {}

---名前でエンティティを取得（見つからない場合も Entity は返る → `:isValid()` で確認）
---@param name string
---@return Entity
function Scene:findEntity(name) end

---モデル（gltf/fbx/obj）を生成
---@param name string
---@param modelPath string assets/ からの相対パス
---@param pos Vec3
---@param rot Vec3 オイラー角（度）
---@param scale Vec3
---@return Entity
function Scene:spawn(name, modelPath, pos, rot, scale) end

---平面（床）を生成
---@param name string
---@param pos Vec3
---@param size number
---@param grid boolean true でグリッド表示
---@return Entity
function Scene:spawnPlane(name, pos, size, grid) end

---箱を生成
---@param name string
---@param pos Vec3
---@param rot Vec3 オイラー角（度）
---@param scale Vec3
---@return Entity
function Scene:spawnBox(name, pos, rot, scale) end

---球を生成
---@param name string
---@param pos Vec3
---@param radius number
---@return Entity
function Scene:spawnSphere(name, pos, radius) end

---エンティティを削除
---@param entity Entity
function Scene:remove(entity) end

---エンティティ総数
---@return integer
function Scene:getEntityCount() end

---UV タイリングを設定（**生成時に1回だけ**。GPU 同期を伴うため毎フレーム禁止）
---@param e Entity
---@param u number
---@param v number
function Scene:setUVScale(e, u, v) end

---頂点カラーで着色（**生成時に1回だけ**。GPU 同期を伴うため毎フレーム禁止。
---instanced メッシュは per-instance 色になり同期なし＝弾などはこちら）
---@param e Entity
---@param r number 0..1
---@param g number 0..1
---@param b number 0..1
function Scene:setColor(e, r, g, b) end

---Sprite2D::effectValue を書き換える（カスタムシェーダー(`shaderPath`)へ渡す汎用の進捗/強度値）。
---頂点属性として補間されるだけなので毎フレーム呼んでもGPU同期無しで安価（ディゾルブ演出の進捗送りなどに）。
---対象エンティティが Sprite2D を持たない場合は何もしない。
---@param e Entity
---@param value number 意味はシェーダー依存（例: 0..1 の消滅/出現進捗）
function Scene:setSpriteEffect(e, value) end

---MeshRenderer::effectValue を書き換える（カスタムシェーダーへ渡す汎用の進捗/強度値、
---Sprite2D::effectValue のメッシュ版）。ルート定数なので毎フレーム呼んでも安価
---（GPU同期・VB再生成なし、マテリアル/テクスチャには一切影響しない）。
---対象エンティティが MeshRenderer を持たない場合は何もしない。
---@param e Entity
---@param value number 意味はシェーダー依存（例: 0..1 の消滅/出現進捗）
function Scene:setMeshEffect(e, value) end

---Sprite2D::shaderParams を書き換える（カスタムシェーダーへ渡す汎用パラメーター4つ、TEXCOORD2）。
---effectValue と同じ頂点属性経路なのでバッチを壊さず毎フレーム呼んでも安価。
---対象エンティティが Sprite2D を持たない場合は何もしない。
---@param e Entity
---@param x number 意味はシェーダー依存
---@param y number
---@param z number
---@param w number
function Scene:setSpriteParams(e, x, y, z, w) end

---Sprite2D の不透明度（color.a）を書き換える（0..1）。半透明フェード演出用。毎フレーム可。
---@param e Entity
---@param alpha number 0..1
function Scene:setSpriteAlpha(e, alpha) end

---Sprite2D の uvMin/uvMax を直接指定する（アトラス切り出しの実行時切替）。
---連番アニメ（animFrames>0）が有効な間は描画時に上書きされるので効かない。
---@param e Entity
---@param u0 number
---@param v0 number
---@param u1 number
---@param v1 number
function Scene:setSpriteUV(e, u0, v0, u1, v1) end

---Sprite2D の UVスクロール速度（単位/秒）を設定する（溶岩表面・滝・背景ループ）。
---連番アニメ（animFrames>0）が有効な間はそちらが優先されて無視される。
---@param e Entity
---@param su number 横スクロール速度
---@param sv number 縦スクロール速度
function Scene:setSpriteScroll(e, su, sv) end

---Sprite2D の連番アニメ（スプライトシート）を設定する。テクスチャを cols x N のグリッドと
---みなし、frame = floor(t*fps) のセルを uvMin/uvMax の代わりに使う。
---**呼ぶたび再生位置が頭に戻る**＝攻撃モーション等の切替がこれ1発で書ける。
---```lua
---scene:setSpriteAnim(player, 8, 12, 8, 1)   -- 8コマ/12fps/横8列/シートの1行目(=walk行)
---scene:setSpriteAnim(player, 0, 0, 0, 0)    -- 停止して uvMin/uvMax 指定に戻す
---```
---@param e Entity
---@param frames integer 総フレーム数（0 で停止し UV 指定に戻る）
---@param fps number 再生速度（フレーム/秒。<=0 は 8 とみなされる）
---@param cols integer シートの列数（0 = frames と同じ = 横1行ストリップ）
---@param row integer シート内の開始行（複数アニメを行で並べたシート用）
function Scene:setSpriteAnim(e, frames, fps, cols, row) end

---Sprite2D の再生モードを設定する。再生位置は頭に戻る。
---@param e Entity
---@param mode integer 0=ループ 1=単発（最終フレームで停止） 2=往復（ピンポン）
function Scene:setSpriteAnimMode(e, mode) end

---Sprite2D の連番アニメを頭から再生し直す（設定は変えない）。
---@param e Entity
function Scene:restartSpriteAnim(e) end

---単発（animMode=1）の再生が終わったか。ループ/往復は常に false。
---爆発スプライトを消す / 次の行動へ進む判定に使う。
---```lua
---if scene:isSpriteAnimDone(explosion) then scene:remove(explosion) end
---```
---@param e Entity
---@return boolean
function Scene:isSpriteAnimDone(e) end

---MeshRenderer::shaderParams を書き換える（カスタムシェーダーへ渡す汎用パラメーター4つ。
---HLSL側は cbuffer PerObjectConstants の effectValue の後ろに float4 shaderParams; を足して読む）。
---ルート定数なので毎フレーム呼んでも安価。対象エンティティが MeshRenderer を持たない場合は何もしない。
---@param e Entity
---@param x number 意味はシェーダー依存
---@param y number
---@param z number
---@param w number
function Scene:setMeshParams(e, x, y, z, w) end

---MeshRenderer の UVスクロール速度（uv/秒）を設定する（滝・溶岩・コンベア・流れる雲）。
---頂点は触らずルート定数へ積むだけなので毎フレーム呼んでも安価（VB再生成なし）。
---uvScaleU/V（UVタイリング）とは併用できる＝タイル済みの UV の上をスクロールする。
---既定の Forward / ForwardSkinned でのみ効く（カスタムシェーダーは自前で b2 を読む必要がある）。
---@param e Entity
---@param su number 横スクロール速度
---@param sv number 縦スクロール速度
function Scene:setMeshUvScroll(e, su, sv) end

---MeshRenderer の連番アニメを設定する（炎/水しぶき/爆発の板ポリ、アニメする看板やモニタ）。
---アルベドを cols x N グリッドとみなしてコマ送りする。有効時は UVタイリング/UVスクロールより優先。
---**呼ぶたび再生位置が頭に戻る**。UV 計算は Sprite2D / UIImage と同じ。
---@param e Entity
---@param frames integer 総フレーム数（0 で停止）
---@param fps number 再生速度（フレーム/秒）
---@param cols integer シートの列数（0 = frames と同じ = 横1行ストリップ）
---@param row integer シート内の開始行
function Scene:setMeshAnim(e, frames, fps, cols, row) end

---MeshRenderer の再生モードを設定する。再生位置は頭に戻る。
---@param e Entity
---@param mode integer 0=ループ 1=単発（最終フレームで停止） 2=往復（ピンポン）
function Scene:setMeshAnimMode(e, mode) end

---MeshRenderer の連番アニメが単発再生を終えたか（ループ/往復は常に false）。
---@param e Entity
---@return boolean
function Scene:isMeshAnimDone(e) end

-- --- ゲーム内UI（retained-mode、UICanvas/UIRect/UIImage/UIText/UIButton）操作 ---
-- UI要素はエディタ（Hierarchy の「UI」サブメニュー等）でツリーを組んで配置し、
-- 以下のヘルパーで実行時に値だけ書き換える。ボタンのクリックは `events:on` で受ける
-- （下の EventBus 節、および `docs/index.html` の「ゲーム内UI（コンポーネント方式）」参照）。
-- 要素が重なった場合は最前面だけが反応する（UIImage.raycastBlock=true（既定）がクリックを遮る。
-- UIText は遮らない。子要素がクリックを吸っても親ボタンへバブリングする）。

---UIText::text を書き換える（スコア・残機・メッセージ）。対象が UIText を持たない場合は何もしない。
---タイプライター（UIText.typewriterSpeed > 0）は文字列が変わると先頭から再生し直す＝会話送りがこれ1発で書ける。
---UIText.rich=true ならテキスト内のインラインタグがスパン装飾として解釈される:
---`[c=RRGGBB]色[/c]`・`[wave]うねり[/wave]`・`[shake]震え[/shake]`・`[rainbow]虹[/rainbow]`
---（入れ子なし・閉じ忘れは文末まで・不正/未知のタグはそのまま表示。wrap とは非両立＝wrap 優先。
---タイプライターの文字数はタグ除去後で数える）。
---```lua
---scene:setUiText(msg, "[c=ff5566]DANGER[/c] を [wave]避けろ[/wave]")
---```
---@param e Entity
---@param text string
function Scene:setUiText(e, text) end

---タイプライター速度（文字/秒）を設定して先頭から再生する。0 で無効＝即全表示（スキップに使える）。
---UTF-8 のコードポイント単位で1文字ずつ現れる（日本語も1文字ずつ）。Play 中のみ進む。
---```lua
---scene:setUiText(msg, "勇者は 荒野を 歩いていた……")
---scene:setUiTypewriter(msg, 20)   -- 20文字/秒
---```
---@param e Entity
---@param charsPerSec number 文字/秒。0 で即全表示
function Scene:setUiTypewriter(e, charsPerSec) end

---タイプライターが全文まで到達したか（会話の「クリックで次へ」判定用）。
---typewriterSpeed=0 や UIText 無しは true。
---```lua
---if keyPressed("SPACE") then
---  if scene:isUiTypewriterDone(msg) then nextLine()
---  else scene:setUiTypewriter(msg, 0) end   -- 途中なら全文表示へスキップ
---end
---```
---@param e Entity
---@return boolean
function Scene:isUiTypewriterDone(e) end

---UIText::text を読む。対象が UIText を持たない場合は空文字列を返す。
---@param e Entity
---@return string
function Scene:getUiText(e) end

---UI要素の色を書き換える（0..1）。UIImage を持っていればそちら優先、無ければ UIText。
---どちらも持たない場合は何もしない。
---@param e Entity
---@param r number 0..1
---@param g number 0..1
---@param b number 0..1
---@param a number 0..1
function Scene:setUiColor(e, r, g, b, a) end

---UI要素の表示/非表示を切り替える。UIRect を持っていればそちら（自身と子孫ごと隠れる）、
---UIRect が無く UICanvas のみ持つ場合（キャンバス自身）は UICanvas.visible を切り替える。
---@param e Entity
---@param visible boolean
function Scene:setUiVisible(e, visible) end

---UIImage::texturePath を差し替える（assets/ からの相対パス）。対象が UIImage を持たない場合は何もしない。
---@param e Entity
---@param path string assets/ からの相対パス
function Scene:setUiTexture(e, path) end

---UIImage の UVスクロール速度（uv/秒）を設定する。uvMax>1（タイル繰り返し）と併用すると
---流れるストライプ / 警告帯 / コンベアのパターンアニメになる。
---連番アニメ（animFrames>0）中と、9-slice / shape≠0 では無効。
---```lua
---scene:setUiUvScroll(warningStripe, 0.5, 0)
---```
---@param e Entity
---@param su number 横スクロール速度
---@param sv number 縦スクロール速度
function Scene:setUiUvScroll(e, su, sv) end

---UIImage の連番アニメ（スプライトシート）を設定する。テクスチャを cols x N のグリッドと
---みなしてコマ送りする（爆発/回復/レベルアップのエフェクト、常時回るローダー）。
---**呼ぶたび再生位置が頭に戻る**。有効中は uvMin/uvMax と uvScroll を無視し、
---9-slice / shape≠0 では無効。エディタ中もプレビュー再生する。
---```lua
---scene:setUiAnim(hitFx, 16, 12, 4, 0)   -- 4x4=16コマを 12fps で
---scene:setUiAnim(hitFx, 0, 0, 0, 0)     -- 停止して uvMin/uvMax 指定に戻す
---```
---@param e Entity
---@param frames integer 総フレーム数（0 で停止し UV 指定に戻る）
---@param fps number 再生速度（フレーム/秒。<=0 は 8 とみなされる）
---@param cols integer シートの列数（0 = frames と同じ = 横1行ストリップ）
---@param row integer シート内の開始行
function Scene:setUiAnim(e, frames, fps, cols, row) end

---UIImage の再生モードを設定する。再生位置は頭に戻る。
---単発=ヒット/回復の演出、往復=待機アイコンの呼吸、ループ=常時回るローダー。
---@param e Entity
---@param mode integer 0=ループ 1=単発（最終フレームで停止） 2=往復（ピンポン）
function Scene:setUiAnimMode(e, mode) end

---UIImage の連番アニメを頭から再生し直す（設定は変えない）。
---@param e Entity
function Scene:restartUiAnim(e) end

---UIImage の連番アニメが単発再生を終えたか（ループ/往復は常に false）。
---```lua
---if scene:isUiAnimDone(hitFx) then scene:setUiVisible(hitFx, false) end
---```
---@param e Entity
---@return boolean
function Scene:isUiAnimDone(e) end

---UIImage::fillAmount（表示割合 0..1）を設定する。HPバー/ゲージ用。
---クリップ方式なのでテクスチャ/9-slice/角丸すべてで「端から現れる」挙動になる
---（方向は Inspector の「Fill 方向」= UIImage.fillDir で設定）。
---```lua
---scene:setUiFill(hpBar, hp / maxHp)
---```
---@param e Entity
---@param amount number 0..1（範囲外はクランプされる）
function Scene:setUiFill(e, amount) end

---UIImage::fillAmount を返す。対象が UIImage を持たない場合は 0。
---@param e Entity
---@return number
function Scene:getUiFill(e) end

---UIRect::rotation（視覚回転・度・時計回り）を設定する。ピボット点回りに掛かり、
---子孫も一緒に回る。レイアウトは軸平行のまま。対象が UIRect を持たない場合は何もしない。
---```lua
---scene:setUiRotation(banner, -8)   -- ペルソナ風の斜めバナー
---```
---@param e Entity
---@param deg number 度（時計回り正）
function Scene:setUiRotation(e, deg) end

---UIRect::rotation を返す。対象が UIRect を持たない場合は 0。
---@param e Entity
---@return number
function Scene:getUiRotation(e) end

---UISlider の現在値（実値 minValue..maxValue）を返す。無ければ 0。
---値の変更はプレイヤー操作時に onChangeEvent（e.value = 実値）で通知される。
---```lua
---events:on("volumeChanged", function(e) audio:setMasterVolume(e.value) end)
---```
---@param e Entity
---@return number
function Scene:getUiSlider(e) end

---UISlider の値を設定する（minValue..maxValue へクランプ）。
---スクリプト起因の変更では onChangeEvent は発火しない（ハンドラの再帰防止）。
---@param e Entity
---@param v number
function Scene:setUiSlider(e, v) end

---UIToggle の状態を返す。無ければ false。
---切替はプレイヤー操作時に onChangeEvent（e.value = 1/0）で通知される。
---@param e Entity
---@return boolean
function Scene:getUiToggle(e) end

---UIToggle の状態を設定する（onChangeEvent は発火しない）。
---@param e Entity
---@param on boolean
function Scene:setUiToggle(e, on) end

---UIScrollView のスクロール量(px)を返す（x, y の2値）。無ければ 0, 0。
---@param e Entity
---@return number x
---@return number y
function Scene:getUiScroll(e) end

---UIScrollView のスクロール量(px)を設定する。翌フレームの描画で
---0..(コンテンツ−ビュー) にクランプされるので、大きい値を入れれば「一番下へ」になる。
---@param e Entity
---@param x number
---@param y number
function Scene:setUiScroll(e, x, y) end

---UI要素をトゥイーンで動かす（イージング付きアニメーション）。対象は UIRect 必須。
---`dx/dy` はレイアウト（UIRect offset）を実際に動かす＝終了位置でクリックも効く。
---`scale/alpha/rotate/color` は見た目だけ（自分と子孫にまとめて掛かる。レイアウトは不変）。
---`scaleX/scaleY` で非等方＝スカッシュ&ストレッチ/フリップ風（scale は両方へ）。
---`color={r,g,b}` は RGB 乗算の絶対目標値（1 超えで白フラッシュ。完了後も持続＝{1,1,1} へ戻して解除）。
---`shake` は振幅px の振動で duration かけて 0 へ減衰（shakeFreq=Hz、既定 24）。
---`fill` は UIImage.fillAmount を絶対目標値へ＝ゲージのなめらか増減（from は現在値）。
---`countTo` は UIText へ数字ロール（カウントアップ演出）。countFrom 省略時は現在テキストの
---数値解釈（＝連続加算が自然に繋がる）。countFmt は printf 書式（"%d"/"%05d"/"%.1f"）。
---`onComplete` は完了フレームに 1 回呼ばれる（SE 同期・演出チェーン用）。
---同時に複数指定でき、連続で呼べば重ねがけもできる。Play 中のみ動く。連打で重なりすぎる
---場面は scene:stopUiTweens(e) で打ち切ってから掛け直す。
---定番演出は uifx.punch / uifx.flash / uifx.countTo 等のワンライナーが楽（プレリュード参照）。
---```lua
----- メニューを右へ 200px、0.4秒 バウンスで
---scene:tweenUi(menu, { dx = 200, duration = 0.4, easing = "bounce" })
----- じわっと消す（消えた後もクリックは残るので hideUi と使い分け）
---scene:tweenUi(popup, { alpha = 0, duration = 0.3 })
----- ダメージで赤フラッシュ + 振動
---scene:tweenUi(hpBar, { color = {3, 0.3, 0.3}, duration = 0.1 })
---scene:tweenUi(hpBar, { color = {1, 1, 1}, duration = 0.25, delay = 0.1 })
---scene:tweenUi(hpBar, { shake = 10, duration = 0.4 })
----- HPゲージをなめらかに減らす
---scene:tweenUi(hpFill, { fill = 0.35, duration = 0.3, easing = "out" })
----- スコアの数字ロール（完了時に SE）
---scene:tweenUi(scoreText, { countTo = 12800, countFmt = "%06d", duration = 0.8,
---                           easing = "expo", onComplete = function() audio:playSFX("se/coin.wav") end })
----- ぺしゃんこから開く（フリップ風）
---scene:tweenUi(card, { scaleY = 1, duration = 0.35, easing = "back" })
---```
---@param e Entity|integer 対象（ボタンクリックの data.source をそのまま渡してもよい）
---@param params { dx?: number, dy?: number, scale?: number, scaleX?: number, scaleY?: number, alpha?: number, rotate?: number, color?: number[], shake?: number, shakeFreq?: number, fill?: number, countTo?: number, countFrom?: number, countFmt?: string, onComplete?: fun(), duration?: number, delay?: number, easing?: "linear"|"in"|"out"|"inOut"|"back"|"bounce"|"elastic"|"expo"|"inBack"|"inOutBack"|"quint"|"sine" }
function Scene:tweenUi(e, params) end

---進行中の tween を全部打ち切り、視覚値（拡縮/透明度/回転/カラー）を既定へ戻す
---（DOTween の Kill 相当。ボタン連打で tween が積み重なるのを防ぐ）。
---UIAnimator の出現/ループアニメには影響しない。
---@param e Entity|integer
function Scene:stopUiTweens(e) end

---UI要素を表示して、UIAnimator の出現アニメを最初から再生する
---（UIAnimator が無ければ setUiVisible(e, true) と同じ）。
---@param e Entity|integer
function Scene:showUi(e) end

---UI要素を出現アニメの逆再生で消す（自分と子孫ごと。消えた後はクリックも通らない）。
---UIAnimator が無い/出現アニメ「なし」なら即座に非表示。戻すのは showUi（setUiVisible では戻らない）。
---@param e Entity|integer
function Scene:hideUi(e) end

---Gimmick コンポーネント付き全エンティティをパラメータ付き配列で返す
---@return GimmickInfo[]
function Scene:gimmicks() end

---タグ一致エンティティの**名前**配列を返す（RTS の群選択など）
---```lua
---for _, name in ipairs(scene:queryByTag("enemy")) do local a = actor(name) end
---```
---@param tag string
---@return string[]
function Scene:queryByTag(tag) end

---XZ 矩形内（＋任意タグ）のエンティティ**名前**配列を返す（矩形選択）
---@param minX number
---@param minZ number
---@param maxX number
---@param maxZ number
---@param tag? string
---@return string[]
function Scene:queryInBox(minX, minZ, maxX, maxZ, tag) end

---シーン（エンティティの生成・検索・クエリ）
---@type Scene
scene = nil

-- ============================================================
-- input: キーボード / マウス
-- ============================================================

---@class Input
local Input = {}

---キーが押されている間ずっと true
---@param vk integer KEY_* 定数
---@return boolean
function Input:isKeyDown(vk) end

---キーを押した瞬間だけ true
---@param vk integer KEY_* 定数
---@return boolean
function Input:isKeyPressed(vk) end

---非同期キー状態（GetAsyncKeyState 相当）
---@param vk integer KEY_* 定数
---@return boolean
function Input:isAsyncKeyDown(vk) end

---マウスキャプチャ中か
---@return boolean
function Input:isMouseCaptured() end

---マウスキャプチャ（カーソル拘束）の ON/OFF
---@param b boolean
function Input:setMouseCapture(b) end

---右クリック中か
---@return boolean
function Input:isRightMouseDown() end

---マウス X 移動量（視点操作用）
---@return number
function Input:getMouseDeltaX() end

---マウス Y 移動量（視点操作用）
---@return number
function Input:getMouseDeltaY() end

---指定パッドが接続されているか
---@param pad integer 0..3
---@return boolean
function Input:isPadConnected(pad) end

---接続中のパッド台数
---@return integer
function Input:getConnectedPadCount() end

---ゲームパッドボタンが押されている間ずっと true
---@param pad integer 0..3
---@param button integer PAD_* 定数
---@return boolean
function Input:isPadButtonDown(pad, button) end

---ゲームパッドボタンを押した瞬間だけ true
---@param pad integer 0..3
---@param button integer PAD_* 定数
---@return boolean
function Input:isPadButtonPressed(pad, button) end

---ゲームパッドボタンを離した瞬間だけ true
---@param pad integer 0..3
---@param button integer PAD_* 定数
---@return boolean
function Input:isPadButtonReleased(pad, button) end

---左スティック X（デッドゾーン適用済み、-1..1）
---@param pad integer 0..3
---@return number
function Input:getPadLeftStickX(pad) end

---左スティック Y（デッドゾーン適用済み、-1..1）
---@param pad integer 0..3
---@return number
function Input:getPadLeftStickY(pad) end

---右スティック X（デッドゾーン適用済み、-1..1）
---@param pad integer 0..3
---@return number
function Input:getPadRightStickX(pad) end

---右スティック Y（デッドゾーン適用済み、-1..1）
---@param pad integer 0..3
---@return number
function Input:getPadRightStickY(pad) end

---左トリガー押し込み量（デッドゾーン適用済み、0..1）
---@param pad integer 0..3
---@return number
function Input:getPadLeftTrigger(pad) end

---右トリガー押し込み量（デッドゾーン適用済み、0..1）
---@param pad integer 0..3
---@return number
function Input:getPadRightTrigger(pad) end

---振動（motor は 0.0..1.0。left=低周波・強モーター、right=高周波・弱モーター）
---@param pad integer 0..3
---@param leftMotor number
---@param rightMotor number
function Input:setPadVibration(pad, leftMotor, rightMotor) end

---seconds 秒後に自動で振動を止める振動（ワンショット/パルス向け）
---@param pad integer 0..3
---@param leftMotor number
---@param rightMotor number
---@param seconds number
function Input:setPadVibrationTimed(pad, leftMotor, rightMotor, seconds) end

---キーボード / マウス / ゲームパッド入力
---@type Input
input = nil

-- ============================================================
-- camera: アクティブカメラの低レベル操作
-- （通常は prelude の cameraFollow / cameraTPS / cameraLockOn を使う）
-- ============================================================

---@class Camera
local Camera = {}

---前後移動
---@param amt number
function Camera:moveForward(amt) end

---左右移動
---@param amt number
function Camera:moveRight(amt) end

---上下移動
---@param amt number
function Camera:moveUp(amt) end

---回転
---@param dyaw number
---@param dpitch number
function Camera:rotate(dyaw, dpitch) end

---@return Vec3
function Camera:getPosition() end

---@param v Vec3
function Camera:setPosition(v) end

---@return number
function Camera:getYaw() end

---@return number
function Camera:getPitch() end

---@param v number
function Camera:setYaw(v) end

---@param v number
function Camera:setPitch(v) end

---@return number
function Camera:getMoveSpeed() end

---@param v number
function Camera:setMoveSpeed(v) end

---@return number
function Camera:getMouseSensitivity() end

---@param v number
function Camera:setMouseSensitivity(v) end

---ワールド座標 → 正規化スクリーン座標（左上原点・0..1）。
---頭上のダメージ数値・名前表示などに（`ui:text(u*SCREEN_W, v*SCREEN_H, ...)`）
---@param x number
---@param y number
---@param z number
---@return number u, number v, boolean visible 背面/画面外は visible=false
function Camera:project(x, y, z) end

---アクティブカメラ
---@type Camera
camera = nil

-- ============================================================
-- audio: BGM / SE / 3D 空間オーディオ
-- ============================================================

---@class AudioSystem
local AudioSystem = {}

---BGM を再生（ループ）
---@param path string assets/ からの相対パス（例: "audio/bgm.wav"）
function AudioSystem:playBGM(path) end

function AudioSystem:stopBGM() end
function AudioSystem:pauseBGM() end
function AudioSystem:resumeBGM() end

---再生中 BGM の位置を秒指定でジャンプ（ループ設定は維持。イントロスキップ等に）
---@param seconds number 曲頭からの秒数（曲長超過はループ内に丸め）
function AudioSystem:seekBGM(seconds) end

---BGM の再生速度倍率（ピッチ連動）。1=通常、0.5=半速+1オクターブ下。スローモ演出用
---@param ratio number 0.05〜2.0 にクランプ。playBGM で新曲開始時に 1.0 へ戻る
function AudioSystem:setBGMRate(ratio) end

---空間SFXのリスナー位置を上書き（プレイヤー中心の定位に）。毎フレーム呼ぶ想定
---@param x number
---@param y number
---@param z number
function AudioSystem:setListener(x, y, z) end

---2D 効果音を再生
---@param path string assets/ からの相対パス
function AudioSystem:playSFX(path) end

---3D 空間効果音を再生（距離減衰）
---@param path string assets/ からの相対パス（モノラル前提）
---@param x number
---@param y number
---@param z number
---@param minD number 減衰開始距離
---@param maxD number 聞こえなくなる距離
---@param vol? number 音量（既定 1.0）
---@param loop? boolean ループ（既定 false）
function AudioSystem:playSpatial(path, x, y, z, minD, maxD, vol, loop) end

---全 SE 停止
function AudioSystem:stopAllSFX() end

---@param v number 0..1
function AudioSystem:setMasterVolume(v) end

---@param v number 0..1
function AudioSystem:setBGMVolume(v) end

---@param v number 0..1
function AudioSystem:setSFXVolume(v) end

---@return number
function AudioSystem:getMasterVolume() end

---@return number
function AudioSystem:getBGMVolume() end

---@return number
function AudioSystem:getSFXVolume() end

---@return string[]
function AudioSystem:getBGMList() end

---@return string[]
function AudioSystem:getSFXList() end

---オーディオフォルダを再スキャン
function AudioSystem:rescan() end

---BGM / SE / 3D 空間オーディオ
---@type AudioSystem
audio = nil

-- ============================================================
-- display: 映像設定（オプション画面用）。set 系は即適用 + settings.json に保存され、
-- ゲームモード起動時に自動適用される（エディタでは自動適用しない）。
-- ============================================================

---@class DisplayApi
local DisplayApi = {}

---垂直同期の ON/OFF
---@param b boolean
function DisplayApi:setVSync(b) end

---@return boolean
function DisplayApi:getVSync() end

---フレームレート上限（VSync OFF 時のみ有効。0=無制限）
---@param fps integer
function DisplayApi:setFpsLimit(fps) end

---@return integer
function DisplayApi:getFpsLimit() end

---画面モード切り替え
---@param mode "windowed"|"borderless"|"fullscreen"
function DisplayApi:setWindowMode(mode) end

---@return "windowed"|"borderless"|"fullscreen"
function DisplayApi:getWindowMode() end

---解像度変更（ウィンドウ=クライアントサイズ / フルスクリーン=ディスプレイモード。ボーダレス中は保存のみ）
---@param w integer
---@param h integer
function DisplayApi:setResolution(w, h) end

---現在の解像度（クライアント領域）
---@return integer w, integer h
function DisplayApi:getResolution() end

---プライマリモニタが対応する解像度一覧（昇順）
---@return {w:integer, h:integer}[]
function DisplayApi:getResolutions() end

---@type DisplayApi
display = nil

-- ============================================================
-- physics: 当たり判定・剛体・キャラクターコントローラ
-- ============================================================

---レイキャスト結果
---@class RaycastHit
---@field hit boolean 当たったか
---@field distance number 距離
---@field point Vec3 衝突点
---@field normal Vec3 法線

---@class PhysicsSystem
local PhysicsSystem = {}

---メッシュ頂点から Convex Hull コライダーを自動生成（既にコライダーがあればスキップ・最大256頂点）
---@param e Entity
function PhysicsSystem:autoCollider(e) end

---ボックスコライダーを追加（既にあればスキップ）
---@param e Entity
---@param hx number half extents X
---@param hy number half extents Y
---@param hz number half extents Z
function PhysicsSystem:addBoxCollider(e, hx, hy, hz) end

---球コライダーを追加（既にあればスキップ）
---@param e Entity
---@param radius number
function PhysicsSystem:addSphereCollider(e, radius) end

---カプセルコライダーを追加（既にあればスキップ）
---@param e Entity
---@param radius number
---@param halfHeight number
function PhysicsSystem:addCapsuleCollider(e, radius, halfHeight) end

---剛体を追加（既にあればスキップ）
---@param e Entity
---@param motionType integer MOTION_STATIC / MOTION_KINEMATIC / MOTION_DYNAMIC
---@param mass number 質量
function PhysicsSystem:addRigidBody(e, motionType, mass) end

---剛体を削除
---@param e Entity
function PhysicsSystem:removeRigidBody(e) end

---力を加える（継続的な押し）
---@param e Entity
---@param force Vec3
function PhysicsSystem:applyForce(e, force) end

---撃力を加える（瞬間的な衝撃・ジャンプやノックバック）
---@param e Entity
---@param impulse Vec3
function PhysicsSystem:applyImpulse(e, impulse) end

---@param e Entity
---@param vel Vec3
function PhysicsSystem:setVelocity(e, vel) end

---@param e Entity
---@return Vec3
function PhysicsSystem:getVelocity(e) end

---物理ボディの位置を直接セット
---@param e Entity
---@param pos Vec3
function PhysicsSystem:setPosition(e, pos) end

---レイキャスト
---```lua
---local hit = physics:raycast(origin, dir, 100)
---if hit.hit then log("距離:", hit.distance) end
---```
---@param origin Vec3
---@param dir Vec3
---@param maxDist number
---@return RaycastHit
function PhysicsSystem:raycast(origin, dir, maxDist) end

---ボックス範囲内のエンティティを列挙
---@param center Vec3
---@param half Vec3 half extents
---@param maxResults? integer 既定 32
---@return Entity[]
function PhysicsSystem:overlapBox(center, half, maxResults) end

---球範囲内のエンティティを列挙
---@param center Vec3
---@param radius number
---@param maxResults? integer 既定 32
---@return Entity[]
function PhysicsSystem:overlapSphere(center, radius, maxResults) end

---物理タイムステップを止める / 再開する
---@param p boolean
function PhysicsSystem:setPaused(p) end

---手動で 1 ステップ進める（pause 中の駒送り用）
---@param dt number
function PhysicsSystem:step(dt) end

---重力ベクトルを設定
---@param g Vec3
function PhysicsSystem:setGravity(g) end

---キャラクターコントローラを付与（既にあればスキップ。RigidBody と排他）
---@param e Entity
---@param radius number
---@param halfHeight number
function PhysicsSystem:addCharacterController(e, radius, halfHeight) end

---水平移動入力（world XZ の目標速度）。**毎フレーム呼ぶ**。呼ばないフレームは停止
---@param e Entity
---@param vx number
---@param vz number
function PhysicsSystem:move(e, vx, vz) end

---ジャンプ要求（接地中のみ有効）
---@param e Entity
---@param amount? number ジャンプ速度（省略/0以下なら既定 jumpSpeed）
function PhysicsSystem:jump(e, amount) end

---接地しているか
---@param e Entity
---@return boolean
function PhysicsSystem:isGrounded(e) end

---物理（Jolt）: 当たり判定・剛体・キャラ操作
---@type PhysicsSystem
physics = nil

---静的（動かない床・壁）
MOTION_STATIC = 0
---キネマティック（スクリプトで動かす）
MOTION_KINEMATIC = 1
---動的（物理シミュレーションに任せる）
MOTION_DYNAMIC = 2

-- ============================================================
-- events: 疎結合イベントバス（Play 中のみ有効）
-- ============================================================

---@class EventBus
local EventBus = {}

---イベントを購読する。**Play 中のみ有効。`OnStart()` 内で登録すること**。
---Trigger の EmitEvent アクションもここに届く。物理接触は
---`engine.contact.enter` / `engine.contact.exit`（data.source / data.other にエンティティID）。
---UIButton は `onClickEvent` に設定した名前で release-inside 確定時に emit される
---（data.source にボタンのエンティティID。data は他キー無し）。
---@param name string イベント名
---@param fn fun(data: table) ハンドラ（data はテーブル。source/other キーあり得る）
---@return integer id `events:off(id)` に渡す購読ID（失敗時 0）
function EventBus:on(name, fn) end

---購読を個別解除（`on` が返した id）
---@param id integer
function EventBus:off(id) end

---イベントを発火。data は `{ key = val, ... }`（number/string/boolean のみ）
---@param name string
---@param data? table
function EventBus:emit(name, data) end

---全購読を解除（Play 開始時に自動 clear される）
function EventBus:clear() end

---疎結合イベントバス
---@type EventBus
events = nil

-- ============================================================
-- net: オンラインマルチプレイ（リッスンサーバー方式・ENetベース）
-- 接続イベントは events:on("net.clientConnected"|"net.clientDisconnected"|"net.hostStarted"|"net.disconnected", fn) で購読
-- ============================================================

---@class NetPlayerInfo
---@field id integer clientId（ホストは0）
---@field rtt integer 往復遅延(ms)
---@field bytesSent number 累積送信バイト数(概算)
---@field bytesReceived number 累積受信バイト数(概算)

---@class Net
local Net = {}

---リッスンサーバーとして待ち受け開始（自分もプレイヤーとして参加）
---@param port? integer 省略時は network.json の defaultPort
---@return string err 空文字列なら成功
function Net:host(port) end

---サーバーへ接続開始。**非同期**（成立/失敗は net.clientConnected / net.disconnected イベントで分かる）
---@param ip string
---@param port? integer 省略時は network.json の defaultPort
---@return string err 空文字列なら「試行開始できた」（接続成立を保証しない）
function Net:join(ip, port) end

---切断（サーバー/クライアントどちらでも呼べる）
function Net:disconnect() end

---@return boolean
function Net:isServer() end
---@return boolean
function Net:isClient() end
---@return boolean
function Net:isConnected() end

---自分の clientId（未接続時は0、ホストは常に0）
---@return integer
function Net:localClientId() end

---接続中のプレイヤー一覧
---@return NetPlayerInfo[]
function Net:players() end

---エンティティをネットワーク複製する（**サーバー専用**）。prefabPath は assets 相対
---（例 "prefabs/Player.prefab"）。実際の生成はフレーム境界(`net.spawned`イベントで分かる)。
---@param prefabPath string
---@param x number
---@param y number
---@param z number
---@param owner? integer 所有者clientId（省略時ホスト=0）
---@return integer netId 採番されたnetId（失敗時0）
---@return string err
function Net:spawn(prefabPath, x, y, z, owner) end

---ネットワーク複製エンティティを破棄する（**サーバー専用**。即時実行）
---@param entity Entity NetworkIdentityを持つエンティティ
---@return string err 空文字列なら成功
function Net:despawn(entity) end

---netId からエンティティを引く（`net.spawned`ハンドラ等から使う）
---@param netId integer
---@return Entity entity 見つからなければ isValid()==false
function Net:findByNetId(netId) end

---@class NetInputCommand
---@field moveX? number
---@field moveZ? number
---@field aimYaw? number
---@field aimPitch? number
---@field buttons? integer ゲーム定義ビットフラグ
---@field jump? boolean

---ローカル入力をサーバーへ送信する（**クライアント専用**）。毎フレーム(OnUpdate)呼ぶこと
---（呼ばなかったフレームは前回値が送られ続ける）
---@param input NetInputCommand
function Net:setInput(input) end

---@class NetInputState
---@field moveX number
---@field moveZ number
---@field aimYaw number
---@field aimPitch number
---@field buttons integer
---@field jump boolean

---entity(NetworkIdentityの`_owner`)の最新入力を読む（**サーバー専用**）
---@param entity Entity
---@return NetInputState
function Net:getInput(entity) end

---サーバーへRPC送信（**クライアント専用**）。引数は number/string/boolean/Vec3 のみ
---@param name string
---@vararg number|string|boolean|Vec3
---@return string err 空文字列なら成功
function Net:rpc(name, ...) end

---全クライアントへRPC送信（**サーバー専用**）
---@param name string
---@vararg number|string|boolean|Vec3
---@return string err
function Net:rpcAll(name, ...) end

---特定クライアントへRPC送信（**サーバー専用**）
---@param clientId integer
---@param name string
---@vararg number|string|boolean|Vec3
---@return string err
function Net:rpcClient(clientId, name, ...) end

---RPCハンドラを登録する。fn(senderClientId, ...)（senderは受信側から見た相手。
---クライアントが受ける場合は常にサーバー=0）
---@param name string
---@param fn fun(sender: integer, ...)
function Net:onRpc(name, fn) end

---マルチプレイ（ENet, リッスンサーバー方式）
---@type Net
net = nil

-- ============================================================
-- ui: ゲーム内 HUD（即時モード・OnUpdate 内で毎フレーム呼ぶ / Play 中のみ）
-- 簡易/デバッグ用の即時 API。恒常的な UI（メニュー・HUD 一式）はエディタで組む
-- retained-mode の UICanvas/UIRect/UIImage/UIText/UIButton（上の Scene:setUi* 参照）を推奨。
-- ============================================================

---@class GameUi
local GameUi = {}

---テキストを描画（スコア・残機・メッセージ）
---@param x number ピクセル（SCREEN_W / SCREEN_H で調整）
---@param y number
---@param text string
---@param size? number フォントサイズ（既定 24）
---@param r? number 0..1（既定 1）
---@param g? number 0..1（既定 1）
---@param b? number 0..1（既定 1）
---@param a? number 0..1（既定 1）
function GameUi:text(x, y, text, size, r, g, b, a) end

---ボタンを描画。**押されたフレームで true** を返す（タイトルメニュー等）
---@param x number
---@param y number
---@param w number
---@param h number
---@param label string
---@return boolean pressed
function GameUi:button(x, y, w, h, label) end

---画像を描画
---@param x number
---@param y number
---@param w number
---@param h number
---@param path string assets/ からの相対パス
function GameUi:image(x, y, w, h, path) end

---塗りつぶし矩形（HP バー・パネル背景）
---@param x number
---@param y number
---@param w number
---@param h number
---@param r? number 0..1（既定 1）
---@param g? number 0..1（既定 1）
---@param b? number 0..1（既定 1）
---@param a? number 0..1（既定 1）
---@param rounding? number 角丸半径（既定 0）
function GameUi:rect(x, y, w, h, r, g, b, a, rounding) end

---ゲーム内 HUD（即時モード）
---@type GameUi
ui = nil

-- ============================================================
-- fx: 即時パーティクル / VFX（座標はワールド、色は 0..1、intensity>1 で白熱＝ブルームで光る）
-- ============================================================

---パーティクルの質感プリセット
---@alias FxKind
---| "glow" 柔らかい光球
---| "fire" fbm 炎（温度ランプ）
---| "smoke" 煙（既定で α ブレンド＝遮蔽）
---| "spark" 火花（速度ストレッチ向き）
---| "magic" 魔法
---| "electric" 放電
---| "lightning" 放電（別名）
---| "ring" 衝撃波リング
---| "shockwave" 衝撃波リング（別名）
---| "star" 閃光
---| "flare" 閃光（別名）
---| integer

---`fx:burst{}` / `fx:ring{}` のパラメータ（全て省略可）
---@class FxBurstParams
---@field x? number 発生位置 X（既定 0）
---@field y? number 発生位置 Y（既定 0.6）
---@field z? number 発生位置 Z（既定 0）
---@field count? integer 粒子数（既定 16）
---@field dx? number 放出方向 X（既定 0）
---@field dy? number 放出方向 Y（既定 1）
---@field dz? number 放出方向 Z（既定 0）
---@field spread? number 拡散 0..1（既定 1）
---@field speed? number 初速（既定 6）
---@field speedVar? number 速度ばらつき 0..1（既定 0.5）
---@field size? number 開始サイズ（既定 0.4）
---@field sizeEnd? number 終了サイズ（既定 0）
---@field sizeMid? number 中間サイズ（>=0 で 3 キー: start→mid→end）
---@field life? number 寿命秒（既定 0.6）
---@field lifeVar? number 寿命ばらつき 0..1（既定 0.3）
---@field r? number 開始色 R 0..1（既定 1）
---@field g? number 開始色 G 0..1（既定 1）
---@field b? number 開始色 B 0..1（既定 1）
---@field rMid? number 中間色 R（指定で 3 キー色）
---@field gMid? number 中間色 G
---@field bMid? number 中間色 B
---@field rEnd? number 終了色 R
---@field gEnd? number 終了色 G
---@field bEnd? number 終了色 B
---@field intensity? number HDR 強度（>1 で白熱・ブルーム発光。既定 3）
---@field gravity? number 重力（負で落下・正で上昇。既定 0）
---@field drag? number 空気抵抗（既定 1）
---@field up? number 上向きバイアス（既定 0）
---@field stretch? number 速度方向ストレッチ（火花の筋。既定 0）
---@field turbStrength? number カールノイズ乱流の強さ（煙/炎の揺らぎ。既定 0）
---@field turbFreq? number 乱流の周波数（既定 1）
---@field kind? FxKind 質感プリセット（既定 "glow"）
---@field blend? integer 0=加算 1=α（既定: smoke のみ 1）
---@field orient? "billboard"|"horizontal"|"vertical"|integer 粒子の向き（既定 "billboard"=カメラ正対。"horizontal"=水平/地面向き、"vertical"=垂直+Z正対。数値 0/1/2 も可。stretch>0 と gpu=true では無効）
---@field flicker? number 明滅の強さ 0..1（既定 0）
---@field flickerFreq? number 明滅の周波数（既定 18）
---@field distort? number >0 で歪みパーティクル（熱ゆらぎ/衝撃波）
---@field light? boolean true で明るい粒子上位をポイントライト化
---@field lightRange? number パーティクルライトの範囲（既定 3）
---@field onDeath? FxBurstParams 粒子の死亡位置に子バースト（サブエミッタ・1段のみ）
---@field ring? boolean onDeath 内で使用: 子をリング放出に
---@field gpu? boolean true で GPU パーティクル（compute・最大131072粒・加算専用）

---`fx:beam{}` のパラメータ
---@class FxBeamParams
---@field x0? number 始点 X
---@field y0? number 始点 Y
---@field z0? number 始点 Z
---@field x1? number 終点 X
---@field y1? number 終点 Y
---@field z1? number 終点 Z
---@field width? number 太さ（既定 0.3）
---@field r? number 0..1（既定 1）
---@field g? number 0..1（既定 1）
---@field b? number 0..1（既定 1）
---@field intensity? number HDR 強度（既定 6）
---@field life? number 寿命秒（既定 0.06。毎フレーム呼べば連続ビーム）
---@field kind? "energy"|"electric"|"lightning"|"fire"|integer ビーム種別（既定 "energy"）

---@class Fx
local Fx = {}

---パーティクルを飛散放出
---```lua
---fx:burst{ x=px, y=1, z=pz, count=20, kind="fire", speed=6,
---          r=1, g=0.5, b=0.1, intensity=5, gravity=-3 }
---```
---@param params FxBurstParams
function Fx:burst(params) end

---リング状（水平円周）に放出（衝撃波）
---@param params FxBurstParams
function Fx:ring(params) end

---画面パルス（被弾演出・クロマ+放射ブラー）
---@param amt? number 強さ（既定 0.5）
function Fx:pulse(amt) end

---全パーティクルを消去
function Fx:clear() end

---ビーム（レーザー/稲妻/火柱）。寿命が短いので**毎フレーム呼ぶ**と連続した一本線になる
---```lua
---fx:beam{ x0=ax, y0=ay, z0=az, x1=bx, y1=by, z1=bz, kind="electric" }
---```
---@param params FxBeamParams
function Fx:beam(params) end

---即時パーティクル放出（低レベル。プリセットは FX.* / vfx.play）
---@type Fx
fx = nil

-- ============================================================
-- グローバル関数: ログ / 永続ストア / シーン遷移
-- ============================================================

---ログ出力（情報）。任意個・任意型の引数を tostring でタブ区切り連結して
---エディタの**コンソールパネル**に出す（`[Lua]` 接頭辞）。Unity の Debug.Log 相当
---```lua
---log("hp:", hp, "pos:", tf.position.x)
---```
---@param ... any
function log(...) end

---警告ログ（コンソールで黄色）
---@param ... any
function logWarn(...) end

---エラーログ（コンソールで赤・コンソールタブが自動で前面に出る）
---@param ... any
function logError(...) end

-- ※ print(...) も log と同じ経路（コンソールパネル）に出るよう差し替え済み

---シーンをまたいで残る数値ストアに保存（スコア・タイムの受け渡し等）
---@param key string
---@param v number
function saveNum(key, v) end

---数値ストアから取得
---@param key string
---@param def? number 無いときの既定値（省略時 0）
---@return number
function loadNum(key, def) end

---ディスク永続の数値ストアに保存（settings.json。再起動しても残る。音量・設定の保存用）
---@param key string
---@param v number
function savePersist(key, v) end

---ディスク永続の数値ストアから取得
---@param key string
---@param def? number 無いときの既定値（省略時 0）
---@return number
function loadPersist(key, def) end

---シーンを即切替（フェード無し）
---@param rel string assets/ からの相対パス（例: "scenes/level1.json"）
function loadScene(rel) end

---シーンが参照するテクスチャ/モデルをキャッシュへ先読み（シーン切替はしない）
---タイトル等で次シーンを先読みしておくと遷移時のカクつきが消える
---@param rel string assets/ からの相対パス（例: "scenes/stage_select.json"）
function preloadScene(rel) end

---SceneFlow（sceneflow.json）の「次のシーン」へ
function nextScene() end

---ゲーム終了
function quit() end

-- --- time: 時間 API（'.' で呼ぶ。C++ 側コア。after/every 等の純Lua拡張はプレリュード参照）---

---@class TimeApi
time = {}

---スケール済み経過秒（setScale の影響を受ける。ポーズ中は止まる）
---@return number
function time.now() end

---実時間の経過秒（setScale の影響を受けない）
---@return number
function time.realtime() end

---スケール済みの今フレーム dt（秒）
---@return number
function time.dt() end

---実時間の今フレーム dt（秒）
---@return number
function time.realDt() end

---フレーム番号
---@return integer
function time.frame() end

---現在のタイムスケール
---@return number
function time.getScale() end

---タイムスケールを設定する。0=ポーズ、0.5=スローモ、2=早送り。
---OnUpdate に渡る dt 自体に掛かるので、既存スクリプトは無改修で追従する。
---UI アニメ(tweenUi/UIAnimator)は実時間で動く＝ポーズ中もポーズメニューは動く。
---@param s number 0 以上
function time.setScale(s) end

---フェード付きシーン切替
---@param rel string assets/ からの相対パス
---@param dur? number フェード秒（既定 0.6）
function fadeToScene(rel, dur) end

---トランジション付きシーン切替
---@param rel string assets/ からの相対パス
---@param type integer 0=フェード 1=ワイプ 2=サークル 3=縦ワイプ
---@param dur? number 秒（既定 0.6）
function transitionToScene(rel, type, dur) end

-- ============================================================
-- グローバル定数
-- ============================================================

---assets ディレクトリの絶対パス
---@type string
ASSETS = nil

---画面幅（ピクセル。リサイズで更新される）
---@type integer
SCREEN_W = nil

---画面高さ（ピクセル。リサイズで更新される）
---@type integer
SCREEN_H = nil

-- キーコード定数（input:isKeyDown / isKeyPressed に渡す。Win32 VK 値）
KEY_A = 65
KEY_B = 66
KEY_C = 67
KEY_D = 68
KEY_E = 69
KEY_F = 70
KEY_G = 71
KEY_H = 72
KEY_I = 73
KEY_J = 74
KEY_K = 75
KEY_L = 76
KEY_M = 77
KEY_N = 78
KEY_O = 79
KEY_P = 80
KEY_Q = 81
KEY_R = 82
KEY_S = 83
KEY_T = 84
KEY_U = 85
KEY_V = 86
KEY_W = 87
KEY_X = 88
KEY_Y = 89
KEY_Z = 90
KEY_0 = 48
KEY_1 = 49
KEY_2 = 50
KEY_3 = 51
KEY_4 = 52
KEY_5 = 53
KEY_6 = 54
KEY_7 = 55
KEY_8 = 56
KEY_9 = 57
KEY_SPACE = 32
KEY_SHIFT = 16
KEY_TAB = 9
KEY_ESCAPE = 27
KEY_ENTER = 13
KEY_UP = 38
KEY_DOWN = 40
KEY_LEFT = 37
KEY_RIGHT = 39
KEY_F1 = 112
KEY_F2 = 113
KEY_F3 = 114
KEY_RBUTTON = 2

-- ゲームパッドボタン定数（isPadButtonDown/Pressed/Released に渡す。値は XINPUT_GAMEPAD_* と同一）
PAD_DPAD_UP    = 0x0001
PAD_DPAD_DOWN  = 0x0002
PAD_DPAD_LEFT  = 0x0004
PAD_DPAD_RIGHT = 0x0008
PAD_START      = 0x0010
PAD_BACK       = 0x0020
PAD_LSTICK     = 0x0040
PAD_RSTICK     = 0x0080
PAD_LB         = 0x0100
PAD_RB         = 0x0200
PAD_A          = 0x1000
PAD_B          = 0x2000
PAD_X          = 0x4000
PAD_Y          = 0x8000

-- ============================================================
-- アタッチスクリプトの規約（properties / OnStart / OnUpdate / self）
-- ============================================================

---`properties = { ... }` の各要素（Inspector に編集欄が出て / シーンに保存され /
---Play 時に `self.<name>` として注入される）
---@class ScriptProperty
---@field name string プロパティ名（self.<name> になる）
---@field type "float"|"int"|"bool"|"string"|"vec3"|"color"|"entity" 型（entity は他エンティティ名で参照、Play 時に Entity 解決）
---@field default any 既定値
---@field min? number スライダー下限
---@field max? number スライダー上限
---@field label? string Inspector 表示名

---スクリプト先頭で宣言する公開プロパティ（任意）
---```lua
---properties = {
---  { name="speed", type="float", default=6, min=0, max=20, label="移動速度" },
---}
---```
---@type ScriptProperty[]
properties = nil

---`OnStart(self)` / `OnUpdate(self, dt)` に渡される self。
---この .lua を貼ったエンティティ自身を指す。
---関数の上に `---@param self ScriptSelf` と書くと self. の補完が効く
---@class ScriptSelf
---@field entity integer エンティティ ID（u32）
---@field name string エンティティ名
---@field transform Transform 自分の Transform
---@field enabled boolean この LuaScript が有効か
---@field [string] any properties で宣言した値・自分で持たせた状態
