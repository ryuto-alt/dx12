// ★生成物。一度きりのブートストラップ。以後は直接編集してよい。再生成すると手編集が消える。
// 生成元: tools/mcp-server/scripts/gen_engine_manifest.mjs（TS の tools/list の凍結コピー + ApplicationMcp*.cpp の McpDefine +
//         engineClient.ts の TIMEOUT_BY_METHOD）。ApplicationMcpManifest.cpp の無名名前空間の中へ include される。
// M(summary, keywords, category, group, target, effect, mode, timeoutMs, idempotent, deferred, dryRun, aliases, params)
// P(name, type, required, enumPipe, min, max, default, desc)
// 行数: 174

void FillManifestChunk0(ManifestRows& o)
{
    o.push_back({"add_particle_layer", M("放出器にレイヤーを 1 枚足す(上限 16 枚)。中身は続けて dx12_set_component(component:'particleEmitter', layer:<index か名前>) で書く。",
        "add particle layer パーティクルのレイヤーを追加",
        "vfx", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "dx12_add_particle_layer", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("layerName", "string", false, nullptr, nullptr, nullptr, nullptr, "レイヤー名(省略で \"Layer N\")。Trigger/Lua から指すのに使うので付けた方がよい。"),
        })});
    o.push_back({"apply_lighting_preset", M("太陽 + ポストプロセスをまとめて『それらしい絵』に振る(冪等)。エディタの「ライティング」窓のプリセットと【同じ実装・同じ値】なので、AI が触った結果と人が押した結果が一致する。",
        "apply lighting preset ライティング・プリセット適用",
        "lighting", "", "", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_apply_lighting_preset", {
            P("preset", "enum", true, "day|dusk|night|indoor|horror|studio", nullptr, nullptr, nullptr, "プリセット名。"),
        })});
    o.push_back({"asset_info", M("アセットの中身情報を GPU を使わず読む。",
        "asset info アセットのメタ情報",
        "asset", "", "", McpEffect::Read, "any", 30000, true, false,
        "none", "dx12_asset_info", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: models/enemy.glb"),
        })});
    o.push_back({"attach_lua_component", M("Lua コンポーネントをエンティティにアタッチする。エディタ上では貼るだけで、実際の初期化/実行は Play 時(OnStart/OnUpdate)。",
        "attach lua component Luaコンポーネントアタッチ",
        "lua", "", "", McpEffect::WriteScene, "any", 8000, false, false,
        "none", "dx12_attach_lua_component", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("script", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: components/Health.lua"),
        })});
    o.push_back({"audio_state", M("今鳴っている音とミキサーの状態を数値で読む(AI が音を観測できる唯一の口)。",
        "audio state 音の状態を観測",
        "audio", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_audio_state", {
        })});
    o.push_back({"benchmark", M("N フレーム(既定300, 30..3600)計測してから統計を返す遅延同期ベンチ。返り値は dx12_perf_stats と同形式 + frames / fps1PercentLow(p99フレーム時間の逆数=スパイク体感指標)。",
        "benchmark ベンチマーク実行",
        "perf", "", "", McpEffect::Runtime, "any", 10000, false, true,
        "none", "dx12_benchmark", {
            P("frames", "int", false, nullptr, nullptr, nullptr, nullptr, "計測フレーム数(既定 300)。30..3600。"),
            P("uncap", "bool", false, nullptr, nullptr, nullptr, nullptr, "計測中だけ FPS上限/VSync を外す(既定 true)。false で普段の設定のまま測る。"),
        })});
    o.push_back({"brain_state", M("Brain コンポーネント(ゲーム AI)の中身を読む。entity / name を省くと Brain を持つエンティティの一覧(今の行動つき)。",
        "brain state ゲームAI(Brain)の状態を観測",
        "ai", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_brain_state", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"build_game", M("現在のプロジェクトをヘッドレスでビルドする(ツールバーの「ビルド」ボタンと同じ処理: exe+DLL+assets+shaders を出力フォルダへコピー)。",
        "build game ゲームビルド",
        "project", "", "", McpEffect::Guarded, "any", 10000, false, false,
        "none", "dx12_build_game", {
        })});
    o.push_back({"create_entity", M("エンティティを生成する(エディタ専用)。フレーム境界で実処理されるが、Node が完了を待って【本物の {entityId, name, sceneGeneration} を同期で返す】({queued} は返りません)。",
        "create entity エンティティ生成",
        "entity", "", "", McpEffect::WriteScene, "editor", 15000, false, true,
        "none", "dx12_create_entity", {
            P("type", "enum", true, "box|sphere|plane|empty|camera|light_directional|light_point|light_spot|particle_emitter|trigger|decal|ui_canvas|ui_image|ui_text|ui_button|ui_slider|ui_toggle|ui_scrollview", nullptr, nullptr, nullptr, "種別。empty は Transform のみ。light_*/camera/particle_emitter/trigger は該当コンポーネント付きで生成(値は既定。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(一意推奨)。省略時は種別名。"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z]。省略時 [0,0,0]。UI 要素では未使用(uiRect で配置)。"),
            P("parent", "int", false, nullptr, nullptr, nullptr, nullptr, "UI 要素の親エンティティ id(ui_canvas 以外で有効)。parentName と排他。"),
            P("parentName", "string", false, nullptr, nullptr, nullptr, nullptr, "UI 要素の親エンティティ名(完全一致)。"),
            P("size", "number", false, nullptr, nullptr, nullptr, nullptr, "type:\"plane\" のときの一辺(m)。0/省略で既定。"),
            P("subdivisions", "int", false, nullptr, nullptr, nullptr, nullptr, "type:\"plane\" の分割数(既定 1 = 4 頂点の板)。★水/海のカスタムシェーダーを貼るなら上げること。4 頂点のままだと頂点を動かす波が一切出ない。"),
        })});
    o.push_back({"create_lua_component", M("Lua コンポーネント(.lua)を assets/components/ に作成する。書き込み前に構文検証され、エラーなら書かず error を返す。",
        "create lua component Luaコンポーネント作成",
        "lua", "", "", McpEffect::WriteFile, "any", 8000, false, false,
        "none", "dx12_create_lua_component", {
            P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "コンポーネント名(拡張子・パス区切りなし)。例: Health"),
            P("code", "string", true, nullptr, nullptr, nullptr, nullptr, "Lua コード全体。properties / OnStart / OnUpdate を含められる。"),
        })});
    o.push_back({"create_prefab", M("エンティティ(+子孫)を .prefab として保存する(Hierarchy 右クリック「プレハブにする」と同じ処理)。",
        "create prefab プレハブ化",
        "entity", "", "", McpEffect::WriteFile, "editor", 10000, false, false,
        "none", "dx12_create_prefab", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets 相対パス(.prefab 必須)。省略時は assets/prefabs/<name>.prefab。"),
        })});
    o.push_back({"create_shader", M("カスタムシェーダー(.hlsl)を assets/shaders/ に作成/上書きする(MeshRenderer::shaderPath 割当用)。",
        "create shader カスタムシェーダー作成",
        "material", "", "", McpEffect::WriteFile, "any", 8000, false, false,
        "none", "dx12_create_shader", {
            P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "シェーダー名(拡張子・パス区切りなし)。例: ToonShade"),
            P("code", "string", false, nullptr, nullptr, nullptr, nullptr, "HLSL コード全体(VSMain/PSMain を含む)。dx12_read_shader で既存のテンプレ/ソースを読んでから書き換えるとよい。template を渡すなら省略できる。"),
            P("template", "string", false, nullptr, nullptr, nullptr, nullptr, "雛形から起こす場合のテンプレート id（water / ocean / particle_ember。一覧は dx12_list_shader_templates）。"),
        })});
}

void FillManifestChunk1(ManifestRows& o)
{
    o.push_back({"debug_human_edit", M("ヘッドレス専用の検証口。人の編集を模して Transform を直接書く（Undo へ人の操作として積む）",
        "debug human edit",
        "undo", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "", {
            P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("position", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"debug_human_undo", M("ヘッドレス専用の検証口。人の Ctrl+Z を模す",
        "debug human undo",
        "undo", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "", {
        })});
    o.push_back({"delete_asset", M("assets 内のファイルを削除する。ディレクトリは recursive=true が必須(誤爆防止)。★シーン/プレハブが参照中のアセットを消すとロードが壊れる。",
        "delete asset アセット削除",
        "asset", "", "", McpEffect::Guarded, "any", 8000, false, false,
        "none", "dx12_delete_asset", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。"),
            P("recursive", "bool", false, nullptr, nullptr, nullptr, nullptr, "ディレクトリを丸ごと消す時に true。既定 false。"),
        })});
    o.push_back({"delete_entity", M("エンティティを子ごと削除する(Undo 可)。entity(id) か name 指定。フレーム境界で実処理され、Node が完了を待って【本物の {deletedEntityId, deletedCount, sceneGeneration} を同期で返す】。",
        "delete entity 削除",
        "entity", "", "", McpEffect::WriteScene, "editor", 15000, false, true,
        "none", "dx12_delete_entity", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"describe_anim_graph", M(".animfsm(アニメーションステートマシン)の構造を返す。",
        "describe anim graph アニメグラフ構造取得",
        "anim", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_describe_anim_graph", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, ".animfsm の assets 相対パス。渡すとエンティティを見ずにファイルを直接パースする(entity/name より優先)。"),
        })});
    o.push_back({"describe_components", M("set_component する前にフィールドを知るための辞書。component 省略で全コンポーネント、指定でそれだけ。",
        "describe components コンポーネント辞書",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_describe_components", {
            P("component", "string", false, nullptr, nullptr, nullptr, nullptr, "特定 jsonKey の定義だけ欲しい時に指定(例 pointLight)。省略で全件。"),
        })});
    o.push_back({"describe_lua_api", M("Lua コンポーネントスクリプトから使えるバインディング一覧を binding ごと(entity/transform/Vec3/self/scene/input/camera/physics/audio/ui/fx/events/globals/prelude)に返す静的辞書。",
        "describe lua api Lua API 辞書",
        "lua", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_describe_lua_api", {
        })});
    o.push_back({"describe_mcp_params", M("エンジン側の MCP ハンドラが【実際に受け付ける引数キーと型】を method 名で引く辞書。エンジンのディスパッチ表(McpDefine の第 2 引数)をそのまま返すので、docs や このサーバの zod スキーマが古くても【エンジンの現物】が分かる。",
        "describe mcp params MCP引数辞書",
        "meta", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_describe_mcp_params", {
            P("method", "string", false, nullptr, nullptr, nullptr, nullptr, "engine の method 名(dx12_ 接頭辞なし。例 \"set_dxr\")。省略で全 method を返す。"),
        })});
    o.push_back({"describe_shader_contract", M("カスタムシェーダーが使える定数(b0/b1)・テクスチャ・入出力・注意点を kind ごとに返す。★cbuffer は【オフセットで対応が決まる】ので、1 つでもズレるとコンパイルは通るのに値だけ化ける(エラーが出ないので気付けない)。",
        "describe shader contract シェーダーの契約を読む",
        "material", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_describe_shader_contract", {
            P("kind", "enum", false, "mesh|particle|sprite|screen", nullptr, nullptr, nullptr, "契約の種別。既定 'mesh'。メッシュ用の雛形を粒に貼っても動かない(別のルートシグネチャ)。"),
        })});
    o.push_back({"diagnose", M("『いま何か壊れてないか？』を 1 回で聞くツール。",
        "diagnose エンジン診断(機械可読)",
        "diag", "", "", McpEffect::Read, "any", 180000, true, false,
        "none", "", {
            P("only", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"duplicate_entity", M("エンティティを子ごとディープ複製する。entity(id) か name 指定。フレーム境界で実処理され、Node が完了を待って【本物の {entityId, name, sceneGeneration} を同期で返す】。",
        "duplicate entity 複製",
        "entity", "", "", McpEffect::WriteScene, "editor", 15000, false, true,
        "none", "dx12_duplicate_entity", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"eval_lua", M("任意の Lua コードをエンジンの Lua state でその場実行する(強力なデバッグ機能)。",
        "eval lua Lua即時実行",
        "lua", "", "", McpEffect::Guarded, "any", 10000, false, false,
        "none", "dx12_eval_lua", {
            P("code", "string", true, nullptr, nullptr, nullptr, nullptr, "実行する Lua コード(複数行可)。"),
        })});
}

void FillManifestChunk2(ManifestRows& o)
{
    o.push_back({"find_entity", M("名前の完全一致でエンティティを1件探す。見つかれば {entityId, name}、無ければ null。",
        "find entity 名前でエンティティ検索",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_find_entity", {
            P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "探すエンティティ名(完全一致)。"),
        })});
    o.push_back({"focus_camera", M("エディタのフライカメラを対象エンティティに寄せる。entity(id) か name 指定。{cameraPos, target, distance} を返す。",
        "focus camera カメラフォーカス",
        "camera", "", "", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_focus_camera", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"get_anim_state", M("エンティティのスケルタルアニメーション情報を返す。{hasSkeletalAnimation, clips:[名前...]}。",
        "get anim state アニメーション状態取得",
        "anim", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_anim_state", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"get_bounds", M("エンティティのワールド空間 AABB を返す。{min, max, center, size, hasMesh}。回転・スケール・親子変換込み。",
        "get bounds ワールドAABB取得",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_bounds", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("includeChildren", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で子孫エンティティの AABB も合成する(モデルルートが empty の時に有効)。"),
            P("perSubmesh", "bool", false, nullptr, nullptr, nullptr, nullptr, "true でサブメッシュ 1 個ずつの内訳を submeshes[] に返す。既定 false。"),
        })});
    o.push_back({"get_contact_shadow", M("現在のシーンのコンタクトシャドウ(深度バッファをスクリーン空間でレイマーチする近接遮蔽)設定を返す。{enabled, rayLength, thickness, bias, intensity, steps, maxDistance, fadeDistance}。",
        "get contact shadow コンタクトシャドウ設定取得",
        "render", "render_setting", "contact_shadow", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_contact_shadow", {
        })});
    o.push_back({"get_depth_prepass", M("深度プリパスの単独強制が ON かを返す。{enabled, note}。",
        "get depth prepass 深度プリパス単独トグル取得",
        "render", "render_setting", "depth_prepass", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_depth_prepass", {
        })});
    o.push_back({"get_dxr", M("現在のシーンの DXR(DirectX Raytracing / inline raytracing)設定と、加速構造の実測値を返す。",
        "get dxr DXR設定取得",
        "render", "render_setting", "dxr", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_dxr", {
        })});
    o.push_back({"get_editor_camera", M("シーンビューを描いてるカメラの状態を返す。",
        "get editor camera エディタカメラ取得",
        "camera", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_editor_camera", {
            P("targetDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "target を再構成する距離(m)。既定 10。0.001〜100000。被写体までの距離を入れると target が実際の注視点に近くなる。"),
        })});
    o.push_back({"get_entity", M("エンティティの全コンポーネントと値を JSON で読む(編集前の状態確認に使う)。entity(id) か name(完全一致)で指定。",
        "get entity エンティティ詳細",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_entity", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"get_hierarchy", M("シーン全体の親子ツリーを返す。{roots:[{entityId, name, children:[...]}], count, sceneGeneration}。",
        "get hierarchy シーン階層ツリー取得",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_hierarchy", {
        })});
    o.push_back({"get_log", M("エンジンログの末尾 N 行を配列で返す。エラーや print() の確認に使う。",
        "get log ログ取得",
        "diag", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_log", {
            P("lines", "int", false, nullptr, nullptr, nullptr, nullptr, "取得行数(既定 50)。"),
        })});
    o.push_back({"get_lua_component_state", M("エンティティの LuaScript の現在のプロパティ値を全部返す(スキーマ基準なので未上書きの既定値も含む。get_entity は保存済みの上書きしか出さない)。",
        "get lua component state Luaプロパティ状態取得",
        "lua", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_lua_component_state", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
}

void FillManifestChunk3(ManifestRows& o)
{
    o.push_back({"get_mode", M("現在のエンジンモード(Editor / Playing)を返す。",
        "get mode モード取得",
        "play", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_mode", {
        })});
    o.push_back({"get_normal_filter", M("法線マップフィルタリング(分散→ラフネス / 平均法線の復元)の設定を返す。{enabled, strength, varianceClamp, geometricBlend}。",
        "get normal filter 法線マップフィルタリング設定取得",
        "render", "render_setting", "normal_filter", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_normal_filter", {
        })});
    o.push_back({"get_occlusion", M("Hi-Z オクルージョンカリングの状態を返す。{enabled, active, ready, pyramid{width,height,mips}}。",
        "get occlusion オクルージョンカリング取得",
        "render", "render_setting", "occlusion", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_occlusion", {
        })});
    o.push_back({"get_physics_state", M("エンティティの物理ランタイム状態(速度・接地判定)を読む。{entityId, hasRigidBody, velocity:[x,y,z], hasCharacterController, isGrounded, world}。entity / name を省くと物理世界の診断 {world:{bodies, max*, updateErrorFlags(上限超過: 1=接触キャッシュ 2=ボディ対 4=接触拘束。超えた分の接触は無視される), failedBodies(作れなかったボディ数), droppedSteps}} だけを返す。床を抜ける / 一部に判定が無い原因はここ。",
        "get physics state 物理ランタイム状態取得",
        "physics", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_physics_state", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"get_play_session", M("直近の Play 1 回ぶんの記録を返す。★dx12_play を押した時点で自動的に記録が始まる(開始ツールは無い)。",
        "get play session 人間のプレイ記録を取る",
        "play", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_play_session", {
            P("maxEvents", "int", false, nullptr, nullptr, nullptr, nullptr, "返すイベント数の上限(既定 400、最大 8000)。新しい方から残す。"),
            P("maxSamples", "int", false, nullptr, nullptr, nullptr, nullptr, "返すサンプル数の上限(既定 200、最大 4000)。新しい方から残す。"),
        })});
    o.push_back({"get_post_process", M("現在のシーンのポストプロセス設定(約25エフェクトの on/off とパラメータ)を全て返す。",
        "get post process ポストプロセス設定取得",
        "render", "render_setting", "post_process", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_post_process", {
        })});
    o.push_back({"get_render_scale", M("内部解像度スケール(レンダー解像度と表示解像度の分離)を返す。{scale, renderResolution{width,height}, displayResolution{width,height}, pending, note}。",
        "get render scale 内部解像度スケール取得",
        "render", "render_setting", "render_scale", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_render_scale", {
        })});
    o.push_back({"get_scene_settings", M("シーンのスカイボックス/IBL・物理大気の設定を返す。{skybox:{envMapPath,iblIntensity,skyboxIntensity,drawSkybox}, decalAtlasPath, atmosphere:{全項目}, atmosphereState:{太陽の高度/方位・IBL 再ベイク回数・GPU 時間}, note}。",
        "get scene settings シーン設定取得",
        "scene", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_scene_settings", {
        })});
    o.push_back({"get_script_errors", M("いま loadError が立っている LuaScript を全部返す: {count, mode, errors:[{entityId,name,scriptPath,message}]}。",
        "get script errors 壊れているLuaを全部出す",
        "lua", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_script_errors", {
        })});
    o.push_back({"get_shadow_pcss", M("現在のシーンの PCSS(ブロッカー探索 → 可変ペナンブラのソフトシャドウ)設定を返す。",
        "get shadow pcss PCSSソフトシャドウ設定取得",
        "render", "render_setting", "shadow_pcss", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_get_shadow_pcss", {
        })});
    o.push_back({"get_ssao", M("現在のシーンの SSAO(スクリーンスペース環境遮蔽)設定を返す。{enabled, radius, bias, intensity, power, sampleCount, blur}。",
        "get ssao SSAO設定取得",
        "render", "render_setting", "ssao", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_ssao", {
        })});
    o.push_back({"get_ssgi", M("現在のシーンの SSGI(スクリーン空間GI)設定を返す。{enabled, intensity, radius, thickness, rayCount, stepCount, clampValue, feedback, iblFallback}。",
        "get ssgi SSGI設定取得",
        "render", "render_setting", "ssgi", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_ssgi", {
        })});
}

void FillManifestChunk4(ManifestRows& o)
{
    o.push_back({"get_ssr", M("現在のシーンの SSR(スクリーン空間反射)設定を返す。{enabled, intensity, maxDistance, thickness, maxSteps, stride, roughnessCutoff, edgeFade, bias}。",
        "get ssr SSR設定取得",
        "render", "render_setting", "ssr", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_ssr", {
        })});
    o.push_back({"get_taa", M("現在のシーンの TAA(テンポラルアンチエイリアス)設定を返す。",
        "get taa TAA設定取得",
        "render", "render_setting", "taa", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_taa", {
        })});
    o.push_back({"get_volumetric_fog", M("現在のシーンのボリュメトリックフォグ(froxel)設定を返す。",
        "get volumetric fog ボリュメトリックフォグ設定取得",
        "render", "render_setting", "volumetric_fog", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_get_volumetric_fog", {
        })});
    o.push_back({"git_branches", M("ローカルブランチの一覧と、今いるブランチを返す: {current, branches[{name,current}]}。",
        "git branches ブランチ一覧",
        "git", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_git_branches", {
        })});
    o.push_back({"git_checkout", M("ブランチを切り替える。create:true なら新規作成して切り替える。",
        "git checkout ブランチ切替/作成",
        "git", "", "", McpEffect::Guarded, "any", 8000, false, false,
        "none", "dx12_git_checkout", {
            P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "ブランチ名。既存へ切り替えるなら dx12_git_branches の名前をそのまま渡す。"),
            P("create", "bool", false, nullptr, nullptr, nullptr, nullptr, "true なら新規作成して切り替える（git checkout -b）。既定 false。"),
        })});
    o.push_back({"git_commit", M("変更を全部ステージしてコミットする（git add -A && git commit）。マージのコンフリクトを解消した後の『マージを終わらせるコミット』もこれ。",
        "git commit コミット",
        "git", "", "", McpEffect::Guarded, "any", 8000, false, false,
        "none", "dx12_git_commit", {
            P("message", "string", true, nullptr, nullptr, nullptr, nullptr, "コミットメッセージ。何を変えたかが分かる 1 行以上。"),
        })});
    o.push_back({"git_fetch", M("リモートの最新を取ってくるだけ（git fetch --all --prune）。作業ツリーは一切変えないので安全。取り込むかどうかは別途 dx12_git_pull / dx12_git_merge で判断する。",
        "git fetch フェッチ",
        "git", "", "", McpEffect::Guarded, "any", 120000, false, false,
        "none", "dx12_git_fetch", {
        })});
    o.push_back({"git_merge", M("name のブランチを『今いるブランチ』へ取り込む（エディタの Git 窓のマージと同じ）。既定は --no-ff＝どこから取り込んだかが履歴に残る。",
        "git merge ブランチをマージ",
        "git", "", "", McpEffect::Guarded, "any", 8000, false, false,
        "none", "dx12_git_merge", {
            P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "取り込み元のブランチ名（dx12_git_branches から取る）。今いるブランチは指定できない。"),
            P("noFastForward", "bool", false, nullptr, nullptr, nullptr, nullptr, "true(既定): --no-ff でマージコミットを必ず作る。false: 早送りできるときは履歴を一直線にする。"),
        })});
    o.push_back({"git_merge_abort", M("進行中のマージを取り消して、始める前の作業ツリーへ戻す（git merge --abort）。コンフリクトを解消しきれないときの逃げ道。",
        "git merge abort マージを中止",
        "git", "", "", McpEffect::Guarded, "any", 8000, false, false,
        "none", "dx12_git_merge_abort", {
        })});
    o.push_back({"git_pull", M("リモートの変更を取り込む（git pull）。コンフリクトすると dx12_git_merge と同じ形（conflicts[] と mergeInProgress:true）で返る。",
        "git pull プル",
        "git", "", "", McpEffect::Guarded, "any", 120000, false, false,
        "none", "dx12_git_pull", {
        })});
    o.push_back({"git_push", M("今のブランチをリモートへ送る（upstream が無ければ -u で設定して送る）。★【外向きの操作】＝押した内容は他人から見える場所へ出る。",
        "git push プッシュ",
        "git", "", "", McpEffect::Guarded, "any", 120000, false, false,
        "none", "dx12_git_push", {
        })});
    o.push_back({"git_status", M("プロジェクトの git 状態を 1 回で返す: {branch, remoteUrl, changedFiles[{path,status,staged}], mergeInProgress, conflicts[], githubUser}。",
        "git status Git 状態",
        "git", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_git_status", {
        })});
}

void FillManifestChunk5(ManifestRows& o)
{
    o.push_back({"group_entities", M("複数エンティティを空の親(グループ)へまとめる。ヒエラルキーの Ctrl+G と同じ。★親は原点・無回転・スケール1で作るので子のワールド位置は動かない(見た目は完全に同じまま)。",
        "group entities グループ化",
        "entity", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "dx12_group_entities", {
            P("entities", "array", false, nullptr, nullptr, nullptr, nullptr, "まとめる エンティティ id の配列。names と併用可。"),
            P("names", "array", false, nullptr, nullptr, nullptr, nullptr, "まとめる エンティティ名(完全一致)の配列。entities と併用可。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "グループ名。省略時 'Group'。重複したら連番が付く。"),
        })});
    o.push_back({"imgui_find", M("ImGui のウィンドウ / ドックのタブ / 名前つき要素を名前で探し、矩形を返す。★座標を推測せず狙うための道具(dx12_imgui_pointer の x,y にそのまま渡せる)。",
        "imgui find UI要素を名前で探す",
        "editor_ui", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_imgui_find", {
            P("label", "string", false, nullptr, nullptr, nullptr, nullptr, "探す名前(ウィンドウ名 / ラベル)。省略で見えているウィンドウ一覧。"),
            P("contains", "bool", false, nullptr, nullptr, nullptr, nullptr, "true(既定)= 部分一致(大小無視)。false = 完全一致。"),
        })});
    o.push_back({"imgui_key", M("仮想入力モードでエディタ UI にキー押下 / 文字入力を送る。",
        "imgui key 仮想キー入力",
        "editor_ui", "", "", McpEffect::Runtime, "any", 30000, false, true,
        "none", "dx12_imgui_key", {
            P("key", "string", false, nullptr, nullptr, nullptr, nullptr, "キー名。\"F2\" / \"Ctrl+S\" / \"Enter\" 等。"),
            P("text", "string", false, nullptr, nullptr, nullptr, nullptr, "入力する文字列(UTF-8)。テキスト欄がアクティブのときに使う。"),
            P("hold", "int", false, nullptr, nullptr, nullptr, nullptr, "key を押し続けるフレーム数(既定 1、最大 600)。エディタのフライカメラの WASD など『押している間だけ効く』操作に。key があるときだけ有効。"),
        })});
    o.push_back({"imgui_pointer", M("仮想入力モード(dx12_imgui_virtual_input)でエディタ UI をポインタ操作する。座標はエディタウィンドウのクライアント座標(px) = dx12_imgui_screenshot の画像ピクセルと同じ。",
        "imgui pointer 仮想ポインタ操作",
        "editor_ui", "", "", McpEffect::Runtime, "any", 90000, false, true,
        "none", "dx12_imgui_pointer", {
            P("action", "enum", true, "move|down|up|click|double_click|drag|wheel", nullptr, nullptr, nullptr, "操作の種類。"),
            P("x", "number", false, nullptr, nullptr, nullptr, nullptr, "クライアント座標 X(px)。move / click / double_click / drag(始点) では必須。down / up / wheel では任意。"),
            P("y", "number", false, nullptr, nullptr, nullptr, nullptr, "クライアント座標 Y(px)。x と対で指定する。"),
            P("button", "enum", false, "left|right|middle", nullptr, nullptr, nullptr, "ボタン。既定 left。"),
            P("toX", "number", false, nullptr, nullptr, nullptr, nullptr, "drag の終点 X(px)。"),
            P("toY", "number", false, nullptr, nullptr, nullptr, nullptr, "drag の終点 Y(px)。"),
            P("steps", "int", false, nullptr, nullptr, nullptr, nullptr, "drag の補間フレーム数(既定 12、1〜600)。"),
            P("dx", "number", false, nullptr, nullptr, nullptr, nullptr, "wheel の横ノッチ数(正 = 右)。"),
            P("dy", "number", false, nullptr, nullptr, nullptr, nullptr, "wheel の縦ノッチ数(正 = 上スクロール)。"),
        })});
    o.push_back({"imgui_screenshot", M("エディタの最終画面を ImGui(パネル・ギズモ・仮想カーソル)込みで PNG に保存して返す。★バックバッファに ImGui を描いた後・Present の前に読み戻すので、PrintWindow を使わない = 窓が背面・画面外・最小化でも撮れ、人の画面には何も出ない。",
        "imgui screenshot ImGui込みスクリーンショット",
        "editor_ui", "", "", McpEffect::Read, "any", 30000, true, true,
        "none", "dx12_imgui_screenshot", {
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "保存先 PNG(絶対パス推奨)。省略で CWD の mcp_imgui_screenshot.png。"),
        })});
    o.push_back({"imgui_virtual_input", M("エディタの『仮想入力モード』を ON/OFF し、現在の状態を返す。★AI がエディタ UI(ImGui)を操作・撮影するときは必ずこのモードを ON にして、起動引数 --background(手前に出さない起動)と併用すること。",
        "imgui virtual input 仮想入力モード切替",
        "editor_ui", "", "", McpEffect::Runtime, "any", 8000, true, false,
        "none", "dx12_imgui_virtual_input", {
            P("enable", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で ON、false で OFF。省略で現在の状態だけ返す。"),
        })});
    o.push_back({"import_asset", M("assets の外にあるファイル/フォルダをプロジェクトの assets/ へコピーする(ダウンロードした素材や /asset コマンドの出力の取り込み用)。",
        "import asset 外部アセット取り込み",
        "asset", "", "", McpEffect::WriteFile, "any", 60000, false, false,
        "none", "dx12_import_asset", {
            P("sourcePath", "string", true, nullptr, nullptr, nullptr, nullptr, "取り込み元の絶対パス(ファイル or フォルダ)。例: C:/Users/me/Downloads/rock.glb"),
            P("destPath", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対の置き先。ファイルなら 'models/rock.glb'、フォルダ/末尾'/' ならその中へ元ファイル名で入る。"),
            P("overwrite", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で既存を上書き。既定 false(存在したらエラー)。"),
        })});
    o.push_back({"key_down", M("キーを押した状態にする(key_up を呼ぶまで保持)。次フレーム以降の Lua input:isKeyDown / keyDown() が true になる。",
        "key down キー押下(保持)",
        "input", "", "", McpEffect::Runtime, "any", 8000, false, false,
        "none", "dx12_key_down", {
            P("key", "any", true, nullptr, nullptr, nullptr, nullptr, "VK コード(int)か キー名(\"W\",\"SPACE\",\"UP\",\"F1\" 等)"),
        })});
    o.push_back({"key_press", M("キーを1フレームだけ押して離す(isKeyPressed / keyPressed() が1回立つ)。ジャンプ(SPACE)などのタップ操作の確認に。",
        "key press キータップ(1フレーム)",
        "input", "", "", McpEffect::Runtime, "any", 8000, false, false,
        "none", "dx12_key_press", {
            P("key", "any", true, nullptr, nullptr, nullptr, nullptr, "VK コード(int)か キー名(\"SPACE\" 等)"),
        })});
    o.push_back({"key_up", M("dx12_key_down で押したキーを離す。key は VK 整数 or 名前。",
        "key up キー離す",
        "input", "", "", McpEffect::Runtime, "any", 8000, false, false,
        "none", "dx12_key_up", {
            P("key", "any", true, nullptr, nullptr, nullptr, nullptr, "VK コード(int)か キー名"),
        })});
    o.push_back({"list_assets", M("assets 配下のアセット一覧 [{path, type, name}] を返す。type で種別フィルタ(省略で全種別)。",
        "list assets アセット一覧",
        "asset", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_list_assets", {
            P("type", "enum", false, "model|texture|script|audio|scene|prefab|shader", nullptr, nullptr, nullptr, "種別フィルタ。省略で全種別。"),
        })});
    o.push_back({"list_entities", M("今開いてるシーンのエンティティ一覧(entityId, name)を返す。verbose で componentTypes も付く。",
        "list entities エンティティ一覧",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_list_entities", {
            P("verbose", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で各エンティティの componentTypes も含める。"),
            P("name_prefix", "string", false, nullptr, nullptr, nullptr, nullptr, "名前の前方一致フィルタ。"),
            P("component_type", "string", false, nullptr, nullptr, nullptr, nullptr, "指定 jsonKey を持つものだけに絞る(例 pointLight)。"),
        })});
}

void FillManifestChunk6(ManifestRows& o)
{
    o.push_back({"list_lights", M("シーンのライトを種別・色・強度・range・コーン角・影の有無つきで列挙する。★同時に【GPU へ送れる灯数の上限に対する使用数】と超過警告を返すのが本命。",
        "list lights ライト一覧と灯数バジェット",
        "lighting", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_list_lights", {
            P("limit", "int", false, nullptr, nullptr, nullptr, nullptr, "1 回に返す件数(既定 50、1..200)。"),
            P("cursor", "int", false, nullptr, nullptr, nullptr, nullptr, "続きを取るときに前回の nextCursor を渡す。"),
        })});
    o.push_back({"list_particle_layers", M("放出器のレイヤーを {index, name, kind, rate, looping, offset, vfxPath} で列挙する。",
        "list particle layers パーティクルのレイヤー一覧",
        "vfx", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_list_particle_layers", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"list_scenes", M("assets/scenes 配下のシーン(.json)一覧 [{path, name}] を返す。dx12_open_scene の path を選ぶのに使う。",
        "list scenes シーン一覧",
        "scene", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_list_scenes", {
        })});
    o.push_back({"list_shader_templates", M("同梱のシェーダー雛形(water / ocean / particle_ember 等)を {name, title, summary} で列挙する。",
        "list shader templates シェーダー雛形一覧",
        "material", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_list_shader_templates", {
        })});
    o.push_back({"look_at", M("エンティティを目標(座標 or 別エンティティ)の方へ回転させる(+Z が正面の想定で rotation Euler を書く)。",
        "look at エンティティを向ける",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_look_at", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("target", "vec3", false, nullptr, nullptr, nullptr, nullptr, "目標のワールド座標 [x,y,z]。targetEntity/targetName と排他。"),
            P("targetEntity", "int", false, nullptr, nullptr, nullptr, nullptr, "目標エンティティ id。"),
            P("targetName", "string", false, nullptr, nullptr, nullptr, nullptr, "目標エンティティ名(完全一致)。"),
            P("upright", "bool", false, nullptr, nullptr, nullptr, nullptr, "true でピッチ 0(水平回転のみ)。キャラや車など直立させたい時。"),
        })});
    o.push_back({"mouse_move", M("合成マウス移動を【次の 1 フレームぶん】注入する。",
        "mouse move マウス移動の注入",
        "input", "", "", McpEffect::Runtime, "any", 10000, false, false,
        "none", "dx12_mouse_move", {
            P("dx", "number", false, nullptr, nullptr, nullptr, nullptr, "水平の移動量(生のピクセル相当)。正で右回り(実装依存)。"),
            P("dy", "number", false, nullptr, nullptr, nullptr, nullptr, "垂直の移動量。正で下(実装依存)。"),
        })});
    o.push_back({"move_asset", M("assets 内のファイル/フォルダを移動・リネームする。",
        "move asset アセット移動/リネーム",
        "asset", "", "", McpEffect::WriteFile, "any", 8000, false, false,
        "none", "dx12_move_asset", {
            P("from", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対の移動元。"),
            P("to", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対の移動先。"),
            P("overwrite", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で既存ファイルを上書き(ディレクトリは不可)。既定 false。"),
            P("updateFiles", "bool", false, nullptr, nullptr, nullptr, nullptr, "ディスク上の他ファイル内の参照も書き換える。既定 true。false にすると開いているシーンのメモリ上だけ更新する。"),
        })});
    o.push_back({"navmesh_build", M("シーンのメッシュからナビメッシュを焼く。★AABB ではなく【実際の三角形】をボクセル化するので、坂道・階段・斜めの壁がそのままの形で反映される。",
        "navmesh build ナビメッシュ生成",
        "navmesh", "navmesh", "build", McpEffect::WriteScene, "editor", 10000, false, false,
        "none", "dx12_navmesh_build", {
            P("cellSize", "number", false, nullptr, nullptr, nullptr, nullptr, "水平ボクセル辺長 m(0.02..2)。エージェント半径の 1/2〜1/3 が目安。小さいほど正確で遅い。"),
            P("cellHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "垂直ボクセル高さ m(0.01..1)。またげる段差の 1/3 以下にすること。"),
            P("agentHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "エージェントの高さ m。頭上クリアランスがこれ未満の場所は歩行不可。"),
            P("agentRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "エージェント半径 m。壁からこのぶん削る。大きすぎると狭い通路が全部消える。"),
            P("agentMaxClimb", "number", false, nullptr, nullptr, nullptr, nullptr, "またげる段差 m(階段一段)。これを超える高低差は崖として切れる。"),
            P("agentMaxSlope", "number", false, nullptr, nullptr, nullptr, nullptr, "★歩ける最大傾斜(度, 0..89)。坂道の許容角。既定 45。"),
            P("minRegionArea", "number", false, nullptr, nullptr, nullptr, nullptr, "m^2。これより小さい孤立した島は捨てる。★箱や柱のような閉じた立体は「底面と天面の間」に頭上クリアランスが空くと内部の床も歩行面として残る(どこからも行けない島になる)。"),
            P("mergeRegionArea", "number", false, nullptr, nullptr, nullptr, nullptr, "m^2。これより小さい領域は隣へ吸収する。経路が素直になる。"),
            P("maxEdgeLen", "number", false, nullptr, nullptr, nullptr, nullptr, "輪郭の辺の最大長 m。0 で無制限。"),
            P("maxSimplificationErr", "number", false, nullptr, nullptr, nullptr, nullptr, "輪郭単純化の許容誤差(ボクセル単位, 既定 1.3)。大きいほどポリゴンが減るが壁に食い込む。"),
            P("maxVertsPerPoly", "int", false, nullptr, nullptr, nullptr, nullptr, "1 ポリゴンの最大頂点数(3..12, 既定 6)。"),
            P("monotonePartition", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で monotone 分割(速いが細長い)。既定 false = 分水嶺(形が良い)。"),
            P("filterLedgeSpans", "bool", false, nullptr, nullptr, nullptr, nullptr, "崖ぎわを歩行不可にする(既定 true)。切ると AI が崖から落ちる経路を引く。"),
            P("filterLowHanging", "bool", false, nullptr, nullptr, nullptr, nullptr, "縁石など低い障害物をまたげるようにする(既定 true)。"),
            P("useBounds", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で boundsMin/Max の範囲だけ焼く。false は全メッシュの AABB。"),
            P("boundsMin", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 焼く範囲の最小。指定すると useBounds が自動で true になる。"),
            P("boundsMax", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 焼く範囲の最大。同上。"),
        })});
    o.push_back({"navmesh_clear", M("焼いたナビメッシュを捨てる。シーンを保存すると隣の .nav も消える。Editor モード限定。",
        "navmesh clear ナビメッシュ破棄",
        "navmesh", "navmesh", "clear", McpEffect::WriteScene, "editor", 10000, true, false,
        "none", "dx12_navmesh_clear", {
        })});
    o.push_back({"navmesh_debug", M("シーンビューに歩行ポリゴンのワイヤを重ねる。明るい線=壁(そこから先へ行けない) / 暗い線=ポリゴン同士のポータル。物理デバッグ描画とは独立にトグルできる。",
        "navmesh debug ナビメッシュ可視化",
        "navmesh", "navmesh", "debug", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_navmesh_debug", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "省略で現在値を返すだけ。"),
        })});
    o.push_back({"navmesh_info", M("現在のナビメッシュの統計(ポリゴン数/頂点数/歩行面積/生成時間/メモリ/範囲)と生成設定を返す。stats.built=false なら未生成。",
        "navmesh info ナビメッシュ情報",
        "navmesh", "navmesh", "info", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_navmesh_info", {
        })});
    o.push_back({"navmesh_path", M("ナビメッシュ上で from → to の経路を求める。A* でポリゴン列を出し、ファネル(string pulling)で通路内の最短折れ線に引き直すので、ポリゴン中心を数珠つなぎにした不自然な形にはならない。",
        "navmesh path 経路探索",
        "navmesh", "navmesh", "path", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_navmesh_path", {
            P("from", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 開始位置。"),
            P("to", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 目標位置。"),
            P("searchRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "指定位置からナビメッシュを探す水平半径 m(既定 max(2, cellSize*8))。"),
            P("searchHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "同じく垂直の許容 m(既定 max(agentHeight*2, 4))。多層フロアで階を取り違えるときは小さくする。"),
        })});
}

void FillManifestChunk7(ManifestRows& o)
{
    o.push_back({"navmesh_raycast", M("from から to へナビメッシュ上を直進し、壁(隣のポリゴンが無い辺)に当たるまでの t と交点/法線を返す。★AABB 近似ではなく実際の輪郭の辺との交差＝『そこまで真っ直ぐ行けるか』の精密な判定。",
        "navmesh raycast ナビメッシュのレイキャスト",
        "navmesh", "navmesh", "raycast", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_navmesh_raycast", {
            P("from", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 始点(ナビメッシュ上か、その近く)。"),
            P("to", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 終点。"),
            P("searchRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "指定位置からナビメッシュを探す水平半径 m(既定 max(2, cellSize*8))。"),
            P("searchHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "同じく垂直の許容 m(既定 max(agentHeight*2, 4))。多層フロアで階を取り違えるときは小さくする。"),
        })});
    o.push_back({"navmesh_sample", M("任意の位置を一番近い歩行面へ落とす。onNavMesh=false なら searchRadius/searchHeight の範囲に歩ける場所が無い。",
        "navmesh sample ナビメッシュへ落とす",
        "navmesh", "navmesh", "sample", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_navmesh_sample", {
            P("point", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 調べる位置。"),
            P("searchRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "指定位置からナビメッシュを探す水平半径 m(既定 max(2, cellSize*8))。"),
            P("searchHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "同じく垂直の許容 m(既定 max(agentHeight*2, 4))。多層フロアで階を取り違えるときは小さくする。"),
        })});
    o.push_back({"navmesh_settings", M("ナビメッシュの生成パラメータだけを変える(焼き直しはしない)。引数なしで現在値を読める。変更後に反映するには dx12_navmesh_build を呼ぶこと。",
        "navmesh settings ナビメッシュ設定",
        "navmesh", "navmesh", "settings", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_navmesh_settings", {
            P("cellSize", "number", false, nullptr, nullptr, nullptr, nullptr, "水平ボクセル辺長 m(0.02..2)。エージェント半径の 1/2〜1/3 が目安。小さいほど正確で遅い。"),
            P("cellHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "垂直ボクセル高さ m(0.01..1)。またげる段差の 1/3 以下にすること。"),
            P("agentHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "エージェントの高さ m。頭上クリアランスがこれ未満の場所は歩行不可。"),
            P("agentRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "エージェント半径 m。壁からこのぶん削る。大きすぎると狭い通路が全部消える。"),
            P("agentMaxClimb", "number", false, nullptr, nullptr, nullptr, nullptr, "またげる段差 m(階段一段)。これを超える高低差は崖として切れる。"),
            P("agentMaxSlope", "number", false, nullptr, nullptr, nullptr, nullptr, "★歩ける最大傾斜(度, 0..89)。坂道の許容角。既定 45。"),
            P("minRegionArea", "number", false, nullptr, nullptr, nullptr, nullptr, "m^2。これより小さい孤立した島は捨てる。★箱や柱のような閉じた立体は「底面と天面の間」に頭上クリアランスが空くと内部の床も歩行面として残る(どこからも行けない島になる)。"),
            P("mergeRegionArea", "number", false, nullptr, nullptr, nullptr, nullptr, "m^2。これより小さい領域は隣へ吸収する。経路が素直になる。"),
            P("maxEdgeLen", "number", false, nullptr, nullptr, nullptr, nullptr, "輪郭の辺の最大長 m。0 で無制限。"),
            P("maxSimplificationErr", "number", false, nullptr, nullptr, nullptr, nullptr, "輪郭単純化の許容誤差(ボクセル単位, 既定 1.3)。大きいほどポリゴンが減るが壁に食い込む。"),
            P("maxVertsPerPoly", "int", false, nullptr, nullptr, nullptr, nullptr, "1 ポリゴンの最大頂点数(3..12, 既定 6)。"),
            P("monotonePartition", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で monotone 分割(速いが細長い)。既定 false = 分水嶺(形が良い)。"),
            P("filterLedgeSpans", "bool", false, nullptr, nullptr, nullptr, nullptr, "崖ぎわを歩行不可にする(既定 true)。切ると AI が崖から落ちる経路を引く。"),
            P("filterLowHanging", "bool", false, nullptr, nullptr, nullptr, nullptr, "縁石など低い障害物をまたげるようにする(既定 true)。"),
            P("useBounds", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で boundsMin/Max の範囲だけ焼く。false は全メッシュの AABB。"),
            P("boundsMin", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 焼く範囲の最小。指定すると useBounds が自動で true になる。"),
            P("boundsMax", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 焼く範囲の最大。同上。"),
        })});
    o.push_back({"net_launch_test_client", M("ホスト中に、同じエンジンをもう1プロセス起動して 127.0.0.1 へ自動接続させる(ツールバーの「テストクライアント起動」ボタンと同じ)。",
        "net launch test client テストクライアント起動",
        "net", "", "", McpEffect::Guarded, "any", 10000, false, false,
        "none", "dx12_net_launch_test_client", {
        })});
    o.push_back({"net_setup", M("次の dx12_play で自動 Host/Join するロールを設定する(ツールバーの Play ロールドロップダウンと同じ)。",
        "net setup ネットワークテストロール設定",
        "net", "", "", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_net_setup", {
            P("role", "enum", true, "host|client|offline", nullptr, nullptr, nullptr, "host=リッスンサーバー / client=address へ接続 / offline=マルチプレイ無効。"),
            P("address", "string", false, nullptr, nullptr, nullptr, nullptr, "client 時の接続先 IP。省略で現状維持(既定 127.0.0.1)。"),
            P("port", "int", false, nullptr, nullptr, nullptr, nullptr, "client 時の接続先ポート。省略/0 でエンジン設定の defaultPort。"),
        })});
    o.push_back({"net_status", M("マルチプレイヤーの現在状態を返す。",
        "net status ネットワーク状態取得",
        "net", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_net_status", {
        })});
    o.push_back({"new_scene", M("新規シーンを作る(現在のシーンを破棄)。savePath を渡すとそのパスに紐づけて作る。フレーム境界で実処理され {applied} を同期で返す。",
        "new scene 新規シーン",
        "scene", "", "", McpEffect::WriteScene, "editor", 45000, false, false,
        "none", "dx12_new_scene", {
            P("savePath", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "新シーンの保存先 assets 相対パス(任意)。"),
        })});
    o.push_back({"open_project", M("プロジェクトを開く(ランチャーのクリックと同等)。path はプロジェクトルートの絶対パス(.dx12proj のあるフォルダ)。",
        "open project プロジェクトを開く",
        "project", "", "", McpEffect::WriteScene, "editor", 10000, false, false,
        "none", "dx12_open_project", {
            P("path", "string", true, nullptr, nullptr, nullptr, nullptr, "プロジェクトルートの絶対パス。例: C:/Users/me/MyGame"),
        })});
    o.push_back({"open_scene", M("シーンを開く(現在のシーンを置換)。path は assets 相対。重い遷移をフレーム境界で実処理し、Node が完了を待って【本物の {sceneName, path, entityCount, sceneGeneration} を同期で返す】。",
        "open scene シーンを開く",
        "scene", "", "", McpEffect::WriteScene, "editor", 45000, false, true,
        "none", "dx12_open_scene", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: scenes/title.json"),
        })});
    o.push_back({"overlap_box", M("center を中心とする箱(半幅 halfExtents・軸平行)と重なっている物理ボディのエンティティを列挙する。★判定は相手の実形状(回した壁・メッシュの凹みも見る。以前の AABB のみ判定ではない)。CharacterController のキャラもカプセルとして含む。同じエンティティは 1 回だけ。★Playing 中のみ意味のある結果。",
        "overlap box ボックス範囲の物理クエリ",
        "physics", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_overlap_box", {
            P("center", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z]"),
            P("halfExtents", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] AABB の半幅。"),
            P("maxResults", "int", false, nullptr, nullptr, nullptr, nullptr, "最大取得数(既定 32、上限 256)。"),
        })});
    o.push_back({"overlap_sphere", M("center を中心とする半径 radius の球と重なっている物理ボディのエンティティを列挙する。★判定は相手の実形状(回した壁・メッシュの凹みも見る。以前の AABB のみ判定ではない)。CharacterController のキャラもカプセルとして含む。同じエンティティは 1 回だけ。★Playing 中のみ意味のある結果。",
        "overlap sphere 球範囲の物理クエリ",
        "physics", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_overlap_sphere", {
            P("center", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z]"),
            P("radius", "number", true, nullptr, nullptr, nullptr, nullptr, "半径。"),
            P("maxResults", "int", false, nullptr, nullptr, nullptr, nullptr, "最大取得数(既定 32、上限 256)。"),
        })});
    o.push_back({"perceive", M("指定の視点から見た画面を ID パスで集計し、【対象がどれくらい見えるか】を数値(raw)と言葉(facts)の両方で返す。",
        "perceive 知覚(プレイヤーの目から見た事実)",
        "capture", "", "", McpEffect::Read, "any", 30000, true, true,
        "none", "dx12_perceive", {
            P("camera", "any", false, nullptr, nullptr, nullptr, nullptr, "\"editor\"(既定)/ \"game\" / {position, target, fovDeg?}。"),
            P("targets", "any", false, nullptr, nullptr, nullptr, nullptr, "対象のエンティティ名(最大 16。親を渡すと子孫ごと 1 対象)。"),
            P("top", "int", false, nullptr, nullptr, nullptr, nullptr, "画面占有の上位を何件返すか(既定 8、0〜64)。"),
            P("width", "int", false, nullptr, nullptr, nullptr, nullptr, "解析の横解像度(省略で表示矩形と同じ。height と両方渡すなら縦横比を合わせる)。"),
            P("height", "int", false, nullptr, nullptr, nullptr, nullptr, "解析の縦解像度。"),
            P("settleFrames", "int", false, nullptr, nullptr, nullptr, nullptr, "決定論モードで落ち着かせるフレーム数(既定 8)。"),
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "最終画を PNG で保存する先(省略で保存しない)。"),
            P("includeTransparent", "bool", false, nullptr, nullptr, nullptr, nullptr, "半透明を「手前の面」として数えるか(既定 true)。"),
            P("camera.fovDeg", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("camera.position", "vec3", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("camera.target", "vec3", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
}

void FillManifestChunk8(ManifestRows& o)
{
    o.push_back({"perf_stats", M("直近 window フレーム(既定60)の性能統計を即時取得。",
        "perf stats パフォーマンス統計",
        "perf", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_perf_stats", {
            P("window", "int", false, nullptr, nullptr, nullptr, nullptr, "平均するフレーム数(既定 60, 最大 240)。"),
        })});
    o.push_back({"pick", M("シーンビューの画面座標から三角形単位でレイキャストして、当たったエンティティを手前から順に返す。",
        "pick 画面座標でピック(三角形精密)",
        "physics", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_pick", {
            P("x", "number", false, nullptr, nullptr, nullptr, nullptr, "ピクセル X(左上原点)。y と対で指定。u/v と排他。"),
            P("y", "number", false, nullptr, nullptr, nullptr, nullptr, "ピクセル Y(左上原点)。x と対で指定。"),
            P("u", "number", false, nullptr, nullptr, nullptr, nullptr, "正規化 X(0..1)。v と対で指定。画面中央は 0.5。"),
            P("v", "number", false, nullptr, nullptr, nullptr, nullptr, "正規化 Y(0..1)。u と対で指定。"),
            P("all", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で重なり全部を手前から返す(既定 false=最前面のみ)。"),
            P("maxHits", "int", false, nullptr, nullptr, nullptr, nullptr, "all:true のときの最大件数(既定 16、上限 64)。"),
            P("includeIcons", "bool", false, nullptr, nullptr, nullptr, nullptr, "ライト/カメラ/空オブジェクトのアイコン当たりを含めるか(既定 true)。"),
            P("trianglePrecise", "bool", false, nullptr, nullptr, nullptr, nullptr, "三角形単位で判定(既定 true)。false でメッシュ AABB 止まり(粗いが速い)。"),
            P("maxCandidates", "int", false, nullptr, nullptr, nullptr, nullptr, "ナローフェーズに掛ける候補数の上限(既定 64)。密集シーンで奥まで拾いたいときだけ上げる。"),
        })});
    o.push_back({"ping", M("エディタとの疎通確認。mode(Editor/Playing)・entityCount・sceneGeneration・currentScene・sceneDirty・protocolVersion を返す。",
        "ping 疎通確認",
        "meta", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_ping", {
        })});
    o.push_back({"play", M("Editor → Playing へ切り替える。フレーム境界で実処理され {mode:'Playing', sceneGeneration, scriptErrors} を同期で返す。",
        "play 再生開始",
        "play", "", "", McpEffect::Runtime, "any", 20000, false, true,
        "none", "dx12_play", {
        })});
    o.push_back({"play_anim", M("スケルタルアニメーションを再生する。★2 つの経路がある: (A) state を渡すと .animfsm ステートマシンの遷移(AnimatorController が必要。ステート名は dx12_describe_anim_graph で確認)。",
        "play anim アニメーション再生",
        "anim", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "dx12_play_anim", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("clip", "int", false, nullptr, nullptr, nullptr, nullptr, "クリップ index。clipName と排他(clipName 優先)。省略時 0。state 指定時は無視。"),
            P("clipName", "string", false, nullptr, nullptr, nullptr, nullptr, "クリップ名(完全一致)。dx12_get_anim_state の clips から選ぶ。state 指定時は無視。"),
            P("blend", "number", false, nullptr, nullptr, nullptr, nullptr, "クロスフェード秒。省略で 0.3。"),
            P("loop", "bool", false, nullptr, nullptr, nullptr, nullptr, "ループ再生するか。省略で現状維持。state 経路では無視。"),
            P("speed", "number", false, nullptr, nullptr, nullptr, nullptr, "再生速度倍率(1.0=等速、2.0=2倍速、0=一時停止)。省略で現状維持。state 経路では無視。"),
            P("state", "string", false, nullptr, nullptr, nullptr, nullptr, ".animfsm のステート名(完全一致)。渡すと clip 経路ではなく FSM の遷移になる。AnimatorController とロード済みグラフが要る。"),
            P("layer", "int", false, nullptr, "0", nullptr, nullptr, "state を遷移させるレイヤー index。省略で 0(ベースレイヤー)。上半身だけ差し替える等のマスク付きレイヤーは 1 以降。"),
        })});
    o.push_back({"project_world_to_screen", M("エンティティのワールド座標を、今シーンビューを描いているカメラで画面ピクセルへ投影する。{x, y, visible, depth, w, width, height, mode}。",
        "project world to screen ワールド→画面投影",
        "camera", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_project_world_to_screen", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"query_entities", M("tag か box のどちらかで複数エンティティを探す(どちらか必須)。box は XZ 平面の矩形 [minX,minZ,maxX,maxZ]。",
        "query entities タグ/領域でエンティティ検索",
        "entity", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_query_entities", {
            P("tag", "string", false, nullptr, nullptr, nullptr, nullptr, "このタグを持つエンティティを列挙。"),
            P("box", "vec4", false, nullptr, nullptr, nullptr, nullptr, "[minX,minZ,maxX,maxZ]。この XZ 矩形に入るエンティティを列挙。"),
        })});
    o.push_back({"raycast", M("origin から direction 方向へ物理レイを飛ばし、最初にヒットしたボディを調べる。CharacterController のキャラ(ボディを持たない)もカプセルとして当たり、結果は {entityId, name, character:true}。始点がキャラのカプセルの中にあるときは、そのキャラは無視される。★Playing 中のみ意味のある結果(Editor 中は body 未登録なので hit=false)。",
        "raycast レイキャスト",
        "physics", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_raycast", {
            P("origin", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] レイの始点。"),
            P("direction", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] レイの方向(正規化不要。エンジン側で正規化される)。"),
            P("maxDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "最大距離(既定 1000)。"),
        })});
    o.push_back({"raycast_precise", M("ワールド空間のレイを飛ばして【描画メッシュの三角形】と交差判定する。返り値は dx12_pick と同形式。★dx12_raycast との違い: あちらは Jolt の物理コライダー基準で Playing 中のみ有効。",
        "raycast precise ワールドレイキャスト(三角形精密)",
        "physics", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_raycast_precise", {
            P("origin", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] レイの始点(ワールド)。"),
            P("direction", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[x,y,z] レイの方向(正規化不要)。真下は [0,-1,0]。"),
            P("maxDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "最大距離(既定 1000)。0 で無制限。"),
            P("all", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で貫通した全ヒットを手前から返す(既定 false=最前面のみ)。"),
            P("maxHits", "int", false, nullptr, nullptr, nullptr, nullptr, "all:true のときの最大件数(既定 16、上限 64)。"),
            P("trianglePrecise", "bool", false, nullptr, nullptr, nullptr, nullptr, "三角形単位で判定(既定 true)。"),
            P("maxCandidates", "int", false, nullptr, nullptr, nullptr, nullptr, "候補数の上限(既定 256)。"),
        })});
    o.push_back({"read_lua_component", M("既存の .lua コンポーネントのソースをそのまま読む。dx12_create_lua_component は新規/上書き書き込み専用で読み取りが無かったため追加。",
        "read lua component Luaコンポーネント読み取り",
        "lua", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_read_lua_component", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: components/Health.lua"),
        })});
    o.push_back({"read_shader", M("既存のカスタムシェーダー(.hlsl)のソースをそのまま読む。dx12_create_shader は新規/上書き書き込み専用で読み取りが無いため、既存シェーダーを確認してから修正版を書き戻す編集ループに使う。",
        "read shader カスタムシェーダー読み取り",
        "material", "", "", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_read_shader", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets/shaders 相対パス。例: ToonShade.hlsl"),
        })});
    o.push_back({"read_texture", M("テクスチャ画像を読み戻して情報（またはピクセルの統計）を返す",
        "read texture",
        "asset", "", "", McpEffect::Read, "any", 15000, true, false,
        "none", "", {
            P("maxSize", "int", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
}

void FillManifestChunk9(ManifestRows& o)
{
    o.push_back({"redo", M("取り消した操作をやり直す(遅延応答)。返り値 {redone, wasAi, onlyAi, redoable, willRedo, next:{undo, redo}}(互換で queuedRedo も残る)。",
        "redo Redo",
        "undo", "", "", McpEffect::WriteScene, "editor", 45000, false, true,
        "none", "dx12_redo", {
            P("onlyAi", "bool", false, nullptr, nullptr, nullptr, nullptr, "true(既定)= AI の操作だけやり直す / false = 人の操作でもやり直す。"),
        })});
    o.push_back({"reload_assets", M("ディスク上で書き換わったアセット(テクスチャ / モデル)のキャッシュを捨てて読み直す: {reloaded, textures[], models[], reboundEntities, skipped[], warnings[]}。",
        "reload assets アセットを読み直す",
        "asset", "", "", McpEffect::Runtime, "any", 10000, false, true,
        "none", "dx12_reload_assets", {
            P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets 相対のファイル or フォルダ。省略で assets 配下ぜんぶ。"),
            P("force", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で更新時刻を見ずに対象を全部読み直す。既定 false(更新時刻が変わったものだけ)。ネットワークドライブ等で更新時刻が当てにならないとき、「書き出したのに 0 件」のときに使う。"),
        })});
    o.push_back({"reload_scripts", M("LuaScript を作り直して loadError を落とす: {reloaded, cleared}。★実行時エラーで死んだスクリプトを Play を止めずに復帰させる用。",
        "reload scripts Luaを強制リロード",
        "lua", "", "", McpEffect::Runtime, "any", 10000, false, false,
        "none", "dx12_reload_scripts", {
            P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets 相対の .lua パス。省略で全 LuaScript が対象"),
        })});
    o.push_back({"remove_component", M("エンティティからコンポーネントを除去する。component は jsonKey。transform/name などコア不変のものは除去不可。",
        "remove component コンポーネント除去",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_remove_component", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("component", "string", true, nullptr, nullptr, nullptr, nullptr, "除去する jsonKey。例: pointLight, rigidBody, boxCollider, sphereCollider, camera, tags"),
        })});
    o.push_back({"remove_particle_layer", M("放出器のレイヤーを 1 枚消す。★最後の 1 枚は消せない(付いているのに何も出ない状態を作らないため)。放出器ごと消すなら dx12_remove_component component='particleEmitter'。",
        "remove particle layer パーティクルのレイヤーを削除",
        "vfx", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "dx12_remove_particle_layer", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("layer", "any", true, nullptr, nullptr, nullptr, nullptr, "消すレイヤーの index(0 始まり)か名前。"),
        })});
    o.push_back({"rename_entity", M("エンティティ名を変更する。重複名は連番(name_2 など)が付与され、確定した {name} を返す。",
        "rename entity リネーム",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_rename_entity", {
            P("entity", "entityRef", true, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。dx12_list_entities / dx12_find_entity で取得。"),
            P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "新しい名前。"),
        })});
    o.push_back({"render_debug", M("レンダラの中間バッファを可視化して PNG 画像で返す。★『絵がなんか変』の原因を切り分けるための唯一の入口。frames フレーム描いてから撮影し、【呼ぶ前と完全に同じ設定へ必ず戻す】(一時的に ON にした機能も戻る)。",
        "render debug 中間バッファ可視化",
        "render", "", "", McpEffect::Read, "any", 60000, true, true,
        "none", "dx12_render_debug", {
            P("mode", "enum", true, "normal|roughness|metallic|depth|ao|contactShadow|velocity|ssr|ssgi|rt|rtDiff|rtAlbedo|vgCluster|vgLod|vgTri|vgDepth|vgOverdraw|vgCoverage|vgMaterial|vgMip|vgTileMaterials|vgNormal|shadowCascade|lightComplexity|clusterGrid|decalCount|fogScattering|fogTransmittance|fogSlice|off", nullptr, nullptr, nullptr, "可視化する中間バッファ。off は『何も撮らず設定を戻すだけ』。albedo / overdraw は非対応。vg* は仮想ジオメトリ（可視性バッファ / 材質 resolve）の検証用で、VG のラスタが動いているときだけ絵が出る。"),
            P("frames", "int", false, nullptr, "1", "120", nullptr, "撮影までに描くフレーム数(1..120、既定 3)。ssr / ssgi は時間蓄積があるので 8〜16 にすると安定する。"),
            P("gain", "number", false, nullptr, nullptr, nullptr, nullptr, "可視化の倍率(既定 1)。velocity は値が小さいので 20 くらいにすると見やすい。"),
            P("depthRange", "number", false, nullptr, nullptr, nullptr, nullptr, "mode:\"depth\" のヒートマップを正規化する距離(m。既定 100)。屋内なら 20、遠景なら 500 等。"),
            P("exposure", "number", false, nullptr, nullptr, nullptr, nullptr, "HDR を出すモード(ssr / ssgi)の露出倍率(既定 1)。真っ黒/真っ白なときに動かす。"),
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "保存先の絶対パス(.png)。省略するとエンジンの CWD へ書く。★ヘッドレス起動で CWD が書けない場所だと『WIC stream open failed』で撮影ごと失敗するので、…"),
        })});
    o.push_back({"save_scene", M("現在のシーンを保存する。path は assets 相対(例 scenes/title.json)。省略時は現在開いてるシーンへ上書き。",
        "save scene シーン保存",
        "scene", "", "", McpEffect::WriteFile, "any", 8000, true, false,
        "none", "dx12_save_scene", {
            P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: scenes/title.json。省略で上書き保存。"),
        })});
    o.push_back({"screenshot", M("今シーンビューに映ってる絵を PNG に書き出して画像で返す(+text に path/width/height/source)。",
        "screenshot スクリーンショット(ポスト前)",
        "capture", "", "", McpEffect::Read, "any", 60000, true, true,
        "none", "dx12_screenshot", {
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "出力先の PNG パス(エンジンの CWD からの相対 or 絶対)。拡張子 .png は自動補完、親フォルダは自動生成、'..' は拒否。★省略すると毎回【同じ既定ファイル】を上書きする。"),
            P("deterministic", "bool", false, nullptr, nullptr, nullptr, nullptr, "true でピクセル完全再現モード。既定 false。time を固定(deband ディザ/グレイン/wave/glitch/パーティクルが止まる)し、…"),
            P("settleFrames", "int", false, nullptr, nullptr, nullptr, nullptr, "deterministic:true のとき履歴を捨ててから回すフレーム数(1..240)。既定 8。増やすと TAA / SSGI の収束が進む(決定性そのものは 8 で得られる)。"),
            P("gizmos", "bool", false, nullptr, nullptr, nullptr, nullptr, "false でこの 1 枚だけエディタのデバッグ描画を止めて撮る。既定 true(従来どおり)。"),
        })});
    o.push_back({"screenshot_final", M("★見た目を判断するときの既定の撮り方。バックバッファ(ポスト適用後の最終画)のビューポート矩形を PNG で返す。",
        "screenshot final 最終画スクリーンショット(ポスト後)",
        "capture", "", "", McpEffect::Read, "any", 60000, true, true,
        "none", "dx12_screenshot_final", {
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "出力先の PNG パス(エンジンの CWD からの相対 or 絶対)。拡張子 .png は自動補完、親フォルダは自動生成、'..' は拒否。★省略すると毎回【同じ既定ファイル】を上書きする。"),
            P("deterministic", "bool", false, nullptr, nullptr, nullptr, nullptr, "true でピクセル完全再現モード。既定 false。time を固定(deband ディザ/グレイン/wave/glitch/パーティクルが止まる)し、…"),
            P("settleFrames", "int", false, nullptr, nullptr, nullptr, nullptr, "deterministic:true のとき履歴を捨ててから回すフレーム数(1..240)。既定 8。増やすと TAA / SSGI の収束が進む(決定性そのものは 8 で得られる)。"),
            P("gizmos", "bool", false, nullptr, nullptr, nullptr, nullptr, "false でこの 1 枚だけエディタのデバッグ描画を止めて撮る。既定 true(従来どおり)。"),
            P("format", "string", false, nullptr, nullptr, nullptr, nullptr, "出力形式: png(既定・8bit) / pfm / exr(ポスト前・露出前の線形 float。物理単位なら nit。tools/parity が読める)。path の拡張子は無視され形式ごとに同じ基準名で書く。"),
            P("formats", "any", false, nullptr, nullptr, nullptr, nullptr, "複数形式を同じフレームで撮る: [\"png\",\"pfm\"] など。応答の files に形式ごとのパスが入る。"),
            P("width", "int", false, nullptr, nullptr, nullptr, nullptr, "オフスクリーン出力の幅(height と両方指定。1 辺 16〜8192・総画素 8192x4096 まで・pfm/exr は 4096x4096 まで)。ウィンドウ/レターボックスと無関係。--size WxH が既定。"),
            P("height", "int", false, nullptr, nullptr, nullptr, nullptr, "任意解像度のオフスクリーン出力の高さ(width と両方指定)。"),
        })});
    o.push_back({"screenshot_game_view", M("アクティブな CameraComponent(ゲームカメラ)視点でシーンを1フレーム描画して PNG で返す。★Editor 中でも Play せずにゲームカメラの見え方(画角・構図)を確認できる。",
        "screenshot game view ゲーム画面スクショ",
        "capture", "", "", McpEffect::Read, "any", 8000, true, true,
        "none", "dx12_screenshot_game_view", {
            P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "保存先の絶対パス(.png)。省略するとエンジンの CWD へ書く。★ヘッドレス起動で CWD が書けない場所だと撮影ごと失敗するので、そのときは指定すること。"),
        })});
    o.push_back({"sculpt_brush", M("スカルプトメッシュの頂点をブラシで動かす。position は【ワールド座標】で渡す(dx12_pick / dx12_raycast_precise の worldPos をそのまま渡すのが確実)。",
        "sculpt brush スカルプトを彫る",
        "terrain", "sculpt", "brush", McpEffect::WriteScene, "editor", 8000, false, false,
        "none", "dx12_sculpt_brush", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("brush", "enum", false, "draw|pull|push|smooth|flatten|pinch|noise|grab", nullptr, nullptr, nullptr, "ブラシ種別(既定 draw)。"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] ブラシ中心(ワールド)。localPosition と排他。どちらか必須。"),
            P("localPosition", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] ブラシ中心(メッシュのローカル空間)。position と排他。"),
            P("radius", "number", false, nullptr, nullptr, nullptr, nullptr, "ブラシ半径(ローカル単位。既定 0.5)。"),
            P("strength", "number", false, nullptr, nullptr, nullptr, nullptr, "1 回ぶんの適用量(既定 0.2)。"),
            P("falloff", "number", false, nullptr, nullptr, nullptr, nullptr, "縁のぼかし 0..1(既定 0.5)。"),
            P("direction", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] pull/push が押し引きする向き(ワールド)。省略時は法線方向。"),
            P("grabDelta", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] brush:grab の移動量(ワールド)。grab では必須。"),
            P("symmetryX", "bool", false, nullptr, nullptr, nullptr, nullptr, "X ミラー対称。"),
            P("symmetryY", "bool", false, nullptr, nullptr, nullptr, nullptr, "Y ミラー対称。"),
            P("symmetryZ", "bool", false, nullptr, nullptr, nullptr, nullptr, "Z ミラー対称。"),
            P("noiseFrequency", "number", false, nullptr, nullptr, nullptr, nullptr, "brush:noise の周波数(既定 1.5)。"),
            P("noiseOctaves", "int", false, nullptr, nullptr, nullptr, nullptr, "brush:noise のオクターブ(1..8)。"),
            P("noiseRidged", "number", false, nullptr, nullptr, nullptr, nullptr, "brush:noise の尾根っぽさ 0..1。"),
            P("seed", "int", false, nullptr, nullptr, nullptr, nullptr, "brush:noise のシード。"),
        })});
}

void FillManifestChunk10(ManifestRows& o)
{
    o.push_back({"sculpt_create", M("彫るための素体メッシュ(box/sphere/plane/cylinder)を作る。岩は sphere、アーチ・柱は cylinder、崖は box が早い。",
        "sculpt create スカルプト素体を作る",
        "terrain", "sculpt", "create", McpEffect::WriteScene, "editor", 45000, true, true,
        "none", "dx12_sculpt_create", {
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(既定 \"Sculpt\")。同名があれば設定更新になる。"),
            P("primitive", "enum", false, "box|sphere|plane|cylinder", nullptr, nullptr, nullptr, "素体の形(既定 sphere)。"),
            P("subdivisions", "int", false, nullptr, nullptr, nullptr, nullptr, "分割数(既定 16、1..64)。細かいほど彫り込めるが重い。"),
            P("size", "number", false, nullptr, nullptr, nullptr, nullptr, "一辺/直径のローカル長 m(既定 2)。"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 配置(既定 [0,0,0])。"),
            P("uvScale", "number", false, nullptr, nullptr, nullptr, nullptr, "UV の倍率(既定 1)。"),
            P("color", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[r,g,b] 0..1 頂点色。"),
            P("collision", "bool", false, nullptr, nullptr, nullptr, nullptr, "彫った形の MeshShape コライダーを作るか(既定 true)。"),
        })});
    o.push_back({"sculpt_make_editable", M("既にシーンにあるモデル(MeshRenderer 持ち)から【彫れるコピー】を作る。元の .glb 等には一切書き戻さない。",
        "sculpt make editable 既存モデルを彫れるようにする",
        "terrain", "sculpt", "make_editable", McpEffect::WriteScene, "editor", 45000, true, true,
        "none", "dx12_sculpt_make_editable", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "できるエンティティの名前(既定 \"<元の名前>_Sculpt\")。"),
        })});
    o.push_back({"select_entity", M("エディタ上で対象エンティティを選択状態にする(Inspector 表示が切り替わる)。entity(id) か name 指定。",
        "select entity 選択",
        "entity", "", "", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_select_entity", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
        })});
    o.push_back({"set_anim_param", M("アニメーション FSM(.animfsm)のパラメータを外から書き換えて遷移を発火させる。",
        "set anim param アニメパラメータ設定",
        "anim", "", "", McpEffect::WriteScene, "any", 10000, false, false,
        "none", "dx12_set_anim_param", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("param", "string", true, nullptr, nullptr, nullptr, nullptr, "FSM パラメータ名(完全一致)。dx12_describe_anim_graph の graph.parameters から選ぶ。"),
            P("value", "any", false, nullptr, nullptr, nullptr, nullptr, "設定する値。Float パラメータなら数値、Bool パラメータなら真偽値。trigger と併用不可(trigger:true が優先)。"),
            P("trigger", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で Trigger パラメータを立てる(値は true 固定。消費は FSM 側)。value の代わりに使う。"),
        })});
    o.push_back({"set_color", M("メッシュの基本色(頂点色の乗算)を設定する。足場やコインの色付けに。color は [r,g,b](0..1)。entity(id) か name 指定。",
        "set color 基本色設定",
        "material", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_color", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("color", "vec3", true, nullptr, nullptr, nullptr, nullptr, "[r,g,b] 0..1。例: 金色=[1,0.84,0]"),
        })});
    o.push_back({"set_component", M("コンポーネントを設定(無ければ追加・あれば置換)。component は jsonKey、data は dx12_describe_components の形。",
        "set component コンポーネント設定",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_component", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("component", "string", true, nullptr, nullptr, nullptr, nullptr, "jsonKey。例: pointLight, directionalLight, spotLight, camera, rigidBody, boxCollider, transform, tags…"),
            P("data", "any", true, nullptr, nullptr, nullptr, nullptr, "コンポーネントの値。オブジェクト or 配列(tags は文字列配列)。dx12_describe_components の fields に合わせる。"),
            P("layer", "any", false, nullptr, nullptr, nullptr, nullptr, "particleEmitter のときだけ有効: 編集するレイヤーの index か名前。省略すると 1 枚目が書き換わる。一覧は dx12_list_particle_layers。"),
            P("values", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"set_contact_shadow", M("コンタクトシャドウのフィールドを指定分だけ更新する(未指定は現状維持)。CSM の解像度では抜ける「物と地面の接地部の細かい影」を補うための機能。",
        "set contact shadow コンタクトシャドウ設定変更",
        "render", "render_setting", "contact_shadow", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_contact_shadow", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("rayLength", "number", false, nullptr, nullptr, nullptr, nullptr, "レイ長(m)。0.1〜0.5 が接触スケール。"),
            P("thickness", "number", false, nullptr, nullptr, nullptr, nullptr, "遮蔽とみなす深度差の上限(m)。"),
            P("bias", "number", false, nullptr, nullptr, nullptr, nullptr, "自己遮蔽バイアス(m)。シミが出るなら上げる。"),
            P("intensity", "number", false, nullptr, nullptr, nullptr, nullptr, "0..1。"),
            P("steps", "int", false, nullptr, nullptr, nullptr, nullptr, "4〜32。既定 16。"),
            P("maxDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "この距離(m)からフェード開始。"),
            P("fadeDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "フェードにかける距離(m)。"),
        })});
    o.push_back({"set_depth_prepass", M("深度プリパスを単独で走らせる。通常は SSAO / コンタクトシャドウ / TAA / SSR / SSGI / DXR のどれかが要求したときだけ走る。",
        "set depth prepass 深度プリパス単独トグル",
        "render", "render_setting", "depth_prepass", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_depth_prepass", {
            P("enabled", "bool", true, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"set_dxr", M("DXR のフィールドを指定分だけ更新する(未指定は現状維持)。",
        "set dxr DXR設定変更",
        "render", "render_setting", "dxr", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_set_dxr", {
            P("shadowEnabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "RT サン影を使うか(既定 false)。ON の間はコンタクトシャドウパスの代わりに RT 影が同じ t11 を埋める。"),
            P("shadowSunAngle", "number", false, nullptr, nullptr, nullptr, nullptr, "太陽の角直径(度)。0..20 にクランプ。既定 0.53(実際の太陽)。0 でハードシャドウ。★PCSS 併用時は 0 が正しい。"),
            P("shadowNormalBias", "number", false, nullptr, nullptr, nullptr, nullptr, "レイ始点の法線方向オフセット(m)。0..1 にクランプ。既定 0.02。アクネ(自己遮蔽の縞)が出るなら上げる。"),
            P("shadowMaxDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "影レイの最大距離(m)。0..100000 にクランプ。既定 0 = 無限。遠景の遮蔽物を追わない分だけ速くなる。"),
            P("shadowIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "RT 影の強さ。0..1 にクランプ。既定 1(RT 影のみ)。0 で無効、途中の値は CSM とのブレンド(デバッグ用)。"),
            P("aoEnabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "RT-AO を使うか(既定 false)。ON の間は SSAO 枠(t8)を RT-AO が埋める。"),
            P("aoRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "半球レイの長さ(m)。0.01..100 にクランプ。既定 1。"),
            P("aoRayCount", "int", false, nullptr, nullptr, nullptr, nullptr, "1px あたりのレイ本数。1..8 にクランプ。既定 2。増やすほど滑らかで重い。"),
            P("aoIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "RT-AO の強さ。0..1 にクランプ。既定 1。"),
            P("aoPower", "number", false, nullptr, nullptr, nullptr, nullptr, "pow() のべき指数(SSAO と同じ意味)。0.01..8 にクランプ。既定 1.5。"),
            P("aoCombineWithSsao", "bool", false, nullptr, nullptr, nullptr, nullptr, "SSAO と min() 合成するか(既定 false)。RT-AO は細かい皺の遮蔽が苦手なので、『大きな遮蔽 = RT / 細部 = SSAO』の合成が実用上いちばん良い。"),
            P("aoDenoise", "bool", false, nullptr, nullptr, nullptr, nullptr, "RT-AO の空間デノイザ(joint bilateral)を使うか(既定 true)。1px 数本のレイをそのまま出すとノイズが乗るので、深度・法線・接平面でエッジを守りながら平滑化する。"),
            P("aoDenoiseRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "デノイザのフィルタ半径(px)。0..32 にクランプ。既定 8。0 で無効。大きいほど滑らかになるが、細い形状の AO が潰れる。"),
            P("maxInstances", "int", false, nullptr, nullptr, nullptr, nullptr, "TLAS へ入れるインスタンスの上限。0..32768 にクランプ。0 で既定(RaytracingScene::kMaxRtInstances)。"),
            P("forceBuildTlas", "bool", false, nullptr, nullptr, nullptr, nullptr, "RT 影 / RT-AO が両方 OFF でも TLAS を建てる(検証用)。シーン JSON には保存されない。"),
            P("ddgiEnabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "DDGI を使うか(既定 false)。プローブ格子にレイを飛ばして八面体 irradiance アトラスを作り、フォワードの ambient に拡散間接項として【加算】する。"),
            P("ddgiSpacing", "number", false, nullptr, nullptr, nullptr, nullptr, "プローブ間隔(m)。0.1..100 にクランプ。既定 2。格子が動くと履歴は捨てられる。"),
            P("ddgiProbeCountX", "int", false, nullptr, nullptr, nullptr, nullptr, "プローブ数 X。1..32 にクランプ。既定 8。"),
            P("ddgiProbeCountY", "int", false, nullptr, nullptr, nullptr, nullptr, "プローブ数 Y。1..32 にクランプ。既定 4。"),
            P("ddgiProbeCountZ", "int", false, nullptr, nullptr, nullptr, nullptr, "プローブ数 Z。1..32 にクランプ。既定 8。"),
            P("ddgiOriginX", "number", false, nullptr, nullptr, nullptr, nullptr, "格子の原点 X(m)。既定 -8。"),
            P("ddgiOriginY", "number", false, nullptr, nullptr, nullptr, nullptr, "格子の原点 Y(m)。既定 0.5。床より少し上に置く。"),
            P("ddgiOriginZ", "number", false, nullptr, nullptr, nullptr, nullptr, "格子の原点 Z(m)。既定 -8。"),
            P("ddgiRayLength", "number", false, nullptr, nullptr, nullptr, nullptr, "プローブレイの最大距離(m)。0.1..10000 にクランプ。既定 30。"),
            P("ddgiHysteresis", "number", false, nullptr, nullptr, nullptr, nullptr, "時間ブレンドの係数。0..0.995 にクランプ。既定 0.97(前フレームを 97% 残す)。大きいほど安定するが光の変化への追従が遅い。0 で毎フレーム入れ替え。"),
            P("ddgiIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "DDGI の強さ。0..10 にクランプ。既定 1。★アトラスへ書き込む時点で掛かるので、変更は ddgiHysteresis ぶんの時間をかけて絵に効く(即時ではない)。0 なら実質 OFF。"),
            P("ddgiNormalBias", "number", false, nullptr, nullptr, nullptr, nullptr, "サンプル位置を法線方向へ押し出す量(m)。0..1 にクランプ。既定 0.02。壁際で裏側のプローブを引いてしまう(ライトリーク)なら上げる。"),
            P("ddgiBounceIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "多重バウンスの強さ。0..1 にクランプ。既定 0 = 1 バウンスのみ(段階2 までと同じ絵)。プローブレイのヒット点で【前フレームのプローブ】を引いて足す量。"),
            P("ddgiFollowCamera", "bool", false, nullptr, nullptr, nullptr, nullptr, "GI モード new のときだけ有効。true = カメラ追従のスクロール格子 + 2 カスケード(近景 ddgiSpacing / 遠景 ddgiSpacing1。ddgiProbeCountX/Y/Z は 1 カスケードの格子数・ddgiOrigin は無視)。false(既定)= 従来の固定ボリューム。"),
            P("ddgiSpacing1", "number", false, nullptr, nullptr, nullptr, nullptr, "遠景カスケードのプローブ間隔(m)。0.1..100 にクランプ。既定 2.0。ddgiFollowCamera:true のときだけ使う。"),
            P("ddgiBudgetMs", "number", false, nullptr, nullptr, nullptr, nullptr, "1 フレームの DDGI の GPU 予算(ms)。0.05..20 にクランプ。既定 1。実測の GPU 時間から更新するプローブ数を決める(超えそうなら間引く)。"),
        })});
    o.push_back({"set_editor_camera", M("シーンビューのカメラを任意視点に置く(focus_camera より自由。俯瞰・引き構図・特定アングルの確認用)。position で位置、target で注視点(yaw/pitch を自動逆算)、または yawDeg/pitchDeg を直接指定。",
        "set editor camera エディタカメラ設定",
        "camera", "", "", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_set_editor_camera", {
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "カメラ位置 [x,y,z]。省略で現在位置のまま。"),
            P("target", "vec3", false, nullptr, nullptr, nullptr, nullptr, "注視点 [x,y,z]。指定すると yaw/pitch を自動計算(yawDeg/pitchDeg より優先)。"),
            P("yawDeg", "number", false, nullptr, nullptr, nullptr, nullptr, "Y軸回転(度)。target 指定時は無視。"),
            P("pitchDeg", "number", false, nullptr, nullptr, nullptr, nullptr, "X軸回転(度、±89 でクランプ)。target 指定時は無視。"),
            P("release", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で Play 中のカメラ固定(overridden)を解除してゲームカメラへ返す。他の引数は無視され {released:true, overridden:false} が返る。"),
        })});
    o.push_back({"set_lua_property", M("LuaScript のプロパティを1つ書き換える(スクリプトの properties 宣言にあるものだけ)。type に応じて value は number/bool/string/[x,y,z]。",
        "set lua property Luaプロパティ設定",
        "lua", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_lua_property", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("key", "string", true, nullptr, nullptr, nullptr, nullptr, "プロパティ名(スクリプトの properties に宣言済みのもの)。"),
            P("value", "any", false, nullptr, nullptr, nullptr, nullptr, "値。型はプロパティに合わせる: number / bool / string / [x,y,z](vec3,color)。"),
        })});
    o.push_back({"set_mesh_shader", M("エンティティの MeshRenderer::shaderPath を設定/解除する(Inspector の「Shader」欄と同じ操作)。",
        "set mesh shader カスタムシェーダー割当",
        "material", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_mesh_shader", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("shaderPath", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets/shaders 相対パス。例: ToonShade.hlsl。省略/空文字で既定 Forward に戻す。"),
            P("alphaBlend", "bool", false, nullptr, nullptr, nullptr, nullptr, "true でシェーダーの alpha 出力を SrcAlpha/InvSrcAlpha ブレンドに使う(DepthWrite OFF)。省略時は既存値を維持、既定は false(不透明固定)。"),
        })});
}

void FillManifestChunk11(ManifestRows& o)
{
    o.push_back({"set_mesh_shader_params", M("カスタムシェーダーの自由枠(b0 の effectValue / shaderParams(float4) / shaderParamsB(float3))へ値を書く。",
        "set mesh shader params カスタムシェーダーのパラメーターを動かす",
        "material", "", "", McpEffect::WriteScene, "any", 10000, true, false,
        "none", "dx12_set_mesh_shader_params", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("effect", "number", false, nullptr, nullptr, nullptr, nullptr, "effectValue(汎用の 1 個目。多くの雛形で『効果の強さ 0..1』)。"),
            P("params", "array", false, nullptr, nullptr, nullptr, nullptr, "shaderParams へ先頭から代入する最大 4 要素の配列。"),
            P("paramsB", "array", false, nullptr, nullptr, nullptr, nullptr, "shaderParamsB へ先頭から代入する最大 3 要素の配列。"),
        })});
    o.push_back({"set_normal_filter", M("法線マップのエイリアシング対策。1 画素の中に法線マップの山谷が何個も入る状況(高いタイリング / 遠景 / 面を舐める角度)で、①鏡面のちらつき ②【光源が面を舐める角度で N·L の符号が裏返り、面が黒い斑点で埋まる】の 2 つが起きる。",
        "set normal filter 法線マップフィルタリング設定変更",
        "render", "render_setting", "normal_filter", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_normal_filter", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "既定 true。false で完全に従来どおり。"),
            P("strength", "number", false, nullptr, nullptr, nullptr, nullptr, "分散のスケール。既定 2。上げるほど強く効く(0 で OFF 相当)。"),
            P("varianceClamp", "number", false, nullptr, nullptr, nullptr, nullptr, "ラフネス α に足せる量の上限。既定 0.25。上げるとよりボケる。"),
            P("geometricBlend", "number", false, nullptr, nullptr, nullptr, nullptr, "分散に応じて幾何法線へ寄せる強さ。既定 2。0 にするとラフネス補正だけになる(黒斑点は残る)。"),
        })});
    o.push_back({"set_occlusion", M("Hi-Z オクルージョンカリングの ON/OFF。深度プリパスの深度から階層 Z ピラミッドを作り、壁の裏に完全に隠れた描画を GPU 側で落とす(D3D12 のプレディケーション。遅延ゼロ)。",
        "set occlusion オクルージョンカリング切替",
        "render", "render_setting", "occlusion", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_occlusion", {
            P("enabled", "bool", true, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"set_parent", M("エンティティの親を設定する。parent 省略で親を解除。サイクルになる指定は拒否。即時反映で ok を返す。",
        "set parent 親子設定",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_parent", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("parent", "int", false, nullptr, nullptr, nullptr, nullptr, "親エンティティ id。省略で親解除。"),
        })});
    o.push_back({"set_pbr", M("エンティティの PBR パラメータ(metallic/roughness/UV スケール/透明/自己発光)を設定する。指定分のみ更新。",
        "set pbr PBR マテリアル設定",
        "material", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_pbr", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("metallic", "number", false, nullptr, nullptr, nullptr, nullptr, "金属度 0..1"),
            P("roughness", "number", false, nullptr, nullptr, nullptr, nullptr, "粗さ 0..1"),
            P("uvScaleU", "number", false, nullptr, nullptr, nullptr, nullptr, "UV の U 方向スケール(タイリング)"),
            P("uvScaleV", "number", false, nullptr, nullptr, nullptr, nullptr, "UV の V 方向スケール(タイリング)"),
            P("alphaMode", "enum", false, "auto|opaque|mask|blend", nullptr, nullptr, nullptr, "透明の扱い。auto=モデルのマテリアル(glTF alphaMode)に従う(既定) / opaque=不透明 / mask=baseColor.a < alphaCutoff を discard…"),
            P("alphaCutoff", "number", false, nullptr, nullptr, nullptr, nullptr, "mask のしきい値 0..1(既定はマテリアル値、glTF 既定 0.5)。負でマテリアルに従う"),
            P("opacity", "number", false, nullptr, nullptr, nullptr, nullptr, "不透明度 0..1。1 未満なら alphaMode を省いても半透明になる(ガラス・水面)"),
            P("emissiveIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "自己発光の強さ 0..64(0=消灯、負=マテリアルに従う)。1 を超えるとブルームが乗る。目安: 看板 2..5 / 天井照明パネル 4..10 / 非常口サイン 3..6"),
            P("emissiveColor", "vec3", false, nullptr, nullptr, nullptr, nullptr, "自己発光の色 [r,g,b](0..1、リニア)。省略して強度だけ指定すると白になる"),
        })});
    o.push_back({"set_post_process", M("ポストプロセスのフィールドを指定分だけ更新する(未指定フィールドは現状維持)。",
        "set post process ポストプロセス設定変更",
        "render", "render_setting", "post_process", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_post_process", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "マスタースイッチ(false で全エフェクト素通し)。"),
            P("tonemapper", "int", false, nullptr, nullptr, nullptr, nullptr, "トーンマッパー: 0=ACES / 1=AgX / 2=なし(ガンマのみ) / 3=UE Filmic(UE 5 の既定の ACES 系。film* で調整) / 4=線形(トーンマップ無し。1 でクリップ) / 5=Khronos PBR Neutral。3〜5 の出力は sRGB OETF(0〜2 は従来どおり pow(1/2.2))。★exposure と共に dx12_screenshot にも反映される(他のグレーディングは反映されない)。"),
            P("filmSlope", "number", false, nullptr, nullptr, nullptr, nullptr, "UE Filmic の Slope(既定 0.88)。tonemapper=3 のときだけ効く。"),
            P("filmToe", "number", false, nullptr, nullptr, nullptr, nullptr, "UE Filmic の Toe(既定 0.55)。"),
            P("filmShoulder", "number", false, nullptr, nullptr, nullptr, nullptr, "UE Filmic の Shoulder(既定 0.26)。"),
            P("filmBlackClip", "number", false, nullptr, nullptr, nullptr, nullptr, "UE Filmic の Black Clip(既定 0)。"),
            P("filmWhiteClip", "number", false, nullptr, nullptr, nullptr, nullptr, "UE Filmic の White Clip(既定 0.04)。"),
            P("exposureMode", "int", false, nullptr, nullptr, nullptr, nullptr, "露出モード: 0=従来(exposureOn/exposure の乗算と autoExposureOn。絵は従来と不変) / 1=手動 EV100(係数 = 1/(1.2·2^(ev100−evComp))。マスター enabled が OFF でも効く) / 2=自動(ヒストグラム測光。平均輝度を 18% グレー×2^evComp へ適応。上下限は aeMinEv100/aeMaxEv100)。"),
            P("ev100", "number", false, nullptr, nullptr, nullptr, nullptr, "手動露出の EV100(既定 15 = 晴天 Sunny 16。EV100=15 + 補正 0 が露出 0 の基準)。exposureMode=1 のとき。1 上げると 1 段暗い。"),
            P("evComp", "number", false, nullptr, nullptr, nullptr, nullptr, "露出補正 [EV](+ で明るく)。exposureMode 1/2 の両方に効く。"),
            P("aeMinEv100", "number", false, nullptr, nullptr, nullptr, nullptr, "自動露出(exposureMode=2)の EV100 下限(既定 -10)。ヒストグラムの測光レンジの下端。"),
            P("aeMaxEv100", "number", false, nullptr, nullptr, nullptr, nullptr, "自動露出(exposureMode=2)の EV100 上限(既定 20)。"),
            P("aeSpeedUp", "number", false, nullptr, nullptr, nullptr, nullptr, "シーンが明るくなる方向の適応速度[1/秒]。0 = aeSpeed と同じ。"),
            P("aeSpeedDown", "number", false, nullptr, nullptr, nullptr, nullptr, "シーンが暗くなる方向の適応速度[1/秒]。0 = aeSpeed と同じ。"),
            P("aeLowPercent", "number", false, nullptr, nullptr, nullptr, nullptr, "測光ヒストグラムの下側の除外割合 0..0.99(既定 0)。0..1 = 全画素の対数平均(平均輝度)、0.8..0.983 = UE のヒストグラム測光の既定。"),
            P("aeHighPercent", "number", false, nullptr, nullptr, nullptr, nullptr, "測光ヒストグラムの上側の上限割合(既定 1)。"),
            P("lightingUnits", "int", false, nullptr, nullptr, nullptr, nullptr, "ライティング単位: 0=従来(既定・絵は不変) / 1=物理(太陽=lux・点/スポット=cd の逆二乗・空/IBL/自己発光=nit。1.0=1 nit)。切替で GPU 待ちが 1 回。"),
            P("exposureOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("exposure", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("contrastOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("contrast", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("brightnessOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("brightness", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("saturationOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("saturation", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("warmthOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("warmth", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("hueOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("hueShift", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("tintOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("tint", "vec3", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("bloomOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("bloom", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("bloomThreshold", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("bloomKnee", "number", false, nullptr, nullptr, nullptr, nullptr, "しきい値のソフト肩。既定 0.5。"),
            P("bloomRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "アップサンプル合成率。大きいほど広く柔らかい。既定 0.65。"),
            P("vignetteOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("vignette", "number", false, nullptr, nullptr, nullptr, nullptr, "減光の濃さ 0..1。"),
            P("vignetteRadius", "number", false, nullptr, nullptr, nullptr, nullptr, "減光が始まる正規化半径(中心=0 / 四隅=1)。既定 0.75。上げるほど四隅だけが落ちる。"),
            P("vignetteSoftness", "number", false, nullptr, nullptr, nullptr, nullptr, "境界のぼけ幅。既定 0.45。小さいほどハッキリした縁。"),
            P("vignetteRoundness", "number", false, nullptr, nullptr, nullptr, nullptr, "1=真円 / 0=画面のアスペクト比なりの楕円。既定 1。"),
            P("vignetteColor", "vec3", false, nullptr, nullptr, nullptr, nullptr, "減光先の色。既定 [0,0,0]（黒）。"),
            P("autoExposureOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "自動露出。ON にすると exposure より優先して効く。"),
            P("aeSpeed", "number", false, nullptr, nullptr, nullptr, nullptr, "適応速度(1/秒)。既定 2。"),
            P("aeEvComp", "number", false, nullptr, nullptr, nullptr, nullptr, "EV 補正(+で明るく)。既定 0。"),
            P("aeLogMin", "number", false, nullptr, nullptr, nullptr, nullptr, "測光レンジ下限(log2 輝度)。既定 -8。"),
            P("aeLogMax", "number", false, nullptr, nullptr, nullptr, nullptr, "測光レンジ上限(log2 輝度)。既定 4。"),
            P("lutOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "3D LUT を有効化。"),
            P("lutPath", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "LUT 画像の assets 相対パス(ストリップ形式 N*N x N。例 1024x32)。"),
            P("lutAmount", "number", false, nullptr, nullptr, nullptr, nullptr, "LUT の適用率 0..1。既定 1。"),
            P("godraysOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "ゴッドレイ(光芒)。★ボリュメトリックフォグと同時に使うと太陽の散乱が二重計上になる。"),
            P("grIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "光条の強さ。既定 0.6。"),
            P("grDensity", "number", false, nullptr, nullptr, nullptr, nullptr, "行進距離(大きいほど長い光条)。既定 0.9。"),
            P("grDecay", "number", false, nullptr, nullptr, nullptr, nullptr, "タップ毎の減衰(1 に近いほど遠くまで伸びる)。既定 0.96。"),
            P("lensflareOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "レンズフレア。bloomOn と併用推奨。"),
            P("lfIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "強度。既定 0.5。"),
            P("lfGhosts", "int", false, nullptr, nullptr, nullptr, nullptr, "ゴースト数 1..8。既定 4。"),
            P("lfDispersal", "number", false, nullptr, nullptr, nullptr, nullptr, "ゴースト間隔。既定 0.35。"),
            P("lfHalo", "number", false, nullptr, nullptr, nullptr, nullptr, "ハロー半径。既定 0.45。"),
            P("lfChroma", "number", false, nullptr, nullptr, nullptr, nullptr, "色収差量。既定 0.01。"),
            P("dofOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "被写界深度。★正射カメラでは効かない。"),
            P("dofFocusDist", "number", false, nullptr, nullptr, nullptr, nullptr, "フォーカス距離(カメラからのビュー距離)。既定 8。"),
            P("dofFocusRange", "number", false, nullptr, nullptr, nullptr, nullptr, "完全にシャープな範囲の広さ。既定 5。"),
            P("dofBlurSize", "number", false, nullptr, nullptr, nullptr, nullptr, "ボケ半径の上限(px)。既定 12。物理モードでは『暴走防止の上限』としてだけ効く。"),
            P("dofFocusName", "string", false, nullptr, nullptr, nullptr, nullptr, "合焦させるエンティティ名(完全一致)。空でなければ毎フレームそのエンティティまでのビュー距離を合焦距離に使う=被写体に合焦したまま寄る/回るが Lua 無しで作れる。"),
            P("dofAperture", "number", false, nullptr, nullptr, nullptr, nullptr, "F 値。既定 2.8(>0 で物理モード: 焦点距離と F 値から錯乱円を出す)。0 以下にすると旧 dofFocusRange 方式へ戻る。"),
            P("dofFocalLength", "number", false, nullptr, nullptr, nullptr, nullptr, "焦点距離(mm)。既定 0 = カメラの FOV から導出(35mm 判・センサ高 24mm 換算)。"),
            P("motionBlurOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "カメラモーションブラー。"),
            P("mbStrength", "number", false, nullptr, nullptr, nullptr, nullptr, "シャッター係数(速度に乗算)。既定 0.5。"),
            P("mbSamples", "int", false, nullptr, nullptr, nullptr, nullptr, "タップ数 4..16。既定 10。"),
            P("chromaticOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("chromatic", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("chromaMode", "int", false, nullptr, nullptr, nullptr, nullptr, "色収差のずらし方: 0=放射(画面端ほど強い) / 1=水平 / 2=垂直。既定 0。"),
            P("pixelizeOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("pixelSize", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("posterizeOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("posterize", "int", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("ditherOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("ditherLevels", "int", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("scanlineOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("scanline", "number", false, nullptr, nullptr, nullptr, nullptr, "走査線の濃さ 0..1。"),
            P("scanCount", "number", false, nullptr, nullptr, nullptr, nullptr, "走査線の本数(画面の縦に何本引くか)。既定 240。"),
            P("scanCurve", "number", false, nullptr, nullptr, nullptr, nullptr, "ブラウン管の画面湾曲の量。既定 0.18。0 で平面(湾曲なし)。"),
            P("sharpenOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("sharpen", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("grainOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("grain", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("grainSize", "number", false, nullptr, nullptr, nullptr, nullptr, "粒の大きさ(px)。既定 1。大きいほど粗い。"),
            P("grainColored", "bool", false, nullptr, nullptr, nullptr, nullptr, "true=RGB 独立のカラーノイズ / false(既定)=輝度ノイズ。"),
            P("invertOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("invert", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("sepiaOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("sepia", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("grayscaleOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("grayscale", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("lensOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("lens", "number", false, nullptr, nullptr, nullptr, nullptr, "歪み量。+ = 樽/魚眼、- = 糸巻き。"),
            P("lensMode", "int", false, nullptr, nullptr, nullptr, nullptr, "0=バレル(多項式 k1/k2) / 1=魚眼(等距離射影) / 2=魚眼(等立体角射影)。既定 0。"),
            P("lensK2", "number", false, nullptr, nullptr, nullptr, nullptr, "2 次の歪み係数(バレルのみ。端だけ余計に曲げる)。既定 0。"),
            P("lensZoom", "number", false, nullptr, nullptr, nullptr, nullptr, "拡大補正。魚眼で四隅が空くときに上げる。既定 1。"),
            P("lensCircular", "bool", false, nullptr, nullptr, nullptr, nullptr, "縦横比を補正して円形に歪ませる。既定 true(false=旧来の UV 空間で横だけ強く歪む)。"),
            P("lensEdge", "int", false, nullptr, nullptr, nullptr, nullptr, "はみ出した所: 0=端を引き伸ばす / 1=黒で塗る / 2=鏡映。既定 1。"),
            P("lensChroma", "number", false, nullptr, nullptr, nullptr, nullptr, "倍率色収差(歪みに比例して RGB がずれる)。既定 0。"),
            P("waveOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("waveAmp", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("waveFreq", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("waveSpeed", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("radialOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("radial", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("radialSamples", "int", false, nullptr, nullptr, nullptr, nullptr, "放射ブラーのタップ数 2..32。既定 8。"),
            P("radialCenterX", "number", false, nullptr, nullptr, nullptr, nullptr, "放射ブラーの中心 X(0..1)。既定 0.5。"),
            P("radialCenterY", "number", false, nullptr, nullptr, nullptr, nullptr, "放射ブラーの中心 Y(0..1)。既定 0.5。"),
            P("glitchOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("glitch", "number", false, nullptr, nullptr, nullptr, nullptr, "横ずれ量。"),
            P("glitchBlocks", "number", false, nullptr, nullptr, nullptr, nullptr, "横帯の本数。既定 24。"),
            P("glitchSpeed", "number", false, nullptr, nullptr, nullptr, nullptr, "崩れが差し替わる速さ(1/秒)。既定 12。"),
            P("glitchColor", "number", false, nullptr, nullptr, nullptr, nullptr, "RGB 分離の量。既定 0.5。0 で色ずれ無し。"),
            P("outlineOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("outline", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("outlineColor", "vec3", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("outlineThickness", "number", false, nullptr, nullptr, nullptr, nullptr, "Sobel のタップ間隔(px)＝線の太さ。既定 1。"),
            P("outlineThreshold", "number", false, nullptr, nullptr, nullptr, nullptr, "これ未満の勾配は線にしない(暗部のノイズ止め)。既定 0.02。"),
            P("outlineOnly", "bool", false, nullptr, nullptr, nullptr, nullptr, "true=絵を捨てて線画だけ描く(下地は outlineBg)。既定 false。"),
            P("outlineBg", "vec3", false, nullptr, nullptr, nullptr, nullptr, "outlineOnly のときの下地色。既定 [1,1,1]（白）。"),
            P("fxaaOn", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("debandOn", "bool", false, nullptr, nullptr, nullptr, nullptr, "8bit 出力のバンディング除去(TPDF ディザ)。既定 ON。切ると空やビネットに縞が出る。"),
        })});
    o.push_back({"set_render_scale", M("3D シーン系の RT(sceneRT / 深度 / SSAO / コンタクトシャドウ / TAA 履歴・速度 / G-Buffer / SSR・SSGI / ブルーム / DoF / ゴッドレイ / 歪み)だけを scale 倍で確保し、最終パスで表示解像度へ引き伸ばす。",
        "set render scale 内部解像度スケール変更",
        "render", "render_setting", "render_scale", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_render_scale", {
            P("scale", "number", true, nullptr, nullptr, nullptr, nullptr, "0.25..1.0。1.0 で等倍(既定)。0.7 くらいから効きが分かる"),
        })});
    o.push_back({"set_scene_settings", M("シーンのスカイボックス/IBL・物理大気・デカールアトラスを設定する。skybox / atmosphere 内の指定フィールドだけ適用。★適用後にエンジンから読み返した実値を current に返す(envMapPath を変えたときは envMapRebake も)。",
        "set scene settings シーン設定変更",
        "scene", "", "", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_set_scene_settings", {
            P("decalAtlasPath", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "デカールアトラス画像の assets 相対パス(RGBA。alpha=覆う度合い)。空文字でデカールを無効化。"),
            P("atmosphere", "object", false, nullptr, nullptr, nullptr, nullptr, "物理大気(既定 OFF)。指定分だけ適用。enabled / preset(earth|mars|haze|twilight)/ timeOfDay / sunMode / driveSun / sunIlluminance ほか。キーは get_scene_settings の atmosphere と同じ。"),
            P("skybox", "object", false, nullptr, nullptr, nullptr, nullptr, "スカイボックス設定。指定したフィールドのみ適用。"),
            P("skybox.drawSkybox", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("skybox.envMapPath", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("skybox.iblIntensity", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("skybox.skyboxIntensity", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"set_shadow_pcss", M("PCSS のフィールドを指定分だけ更新する(未指定は現状維持)。CSM の固定幅 3x3 PCF を「ブロッカー探索 → 距離に応じた可変ペナンブラ」へ置き換える = 接地部は鋭く、離れるほど柔らかい影になる。",
        "set shadow pcss PCSSソフトシャドウ設定変更",
        "render", "render_setting", "shadow_pcss", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_set_shadow_pcss", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "PCSS を使うか。false で従来の 3x3 PCF(絵はビット一致)。"),
            P("lightTanAngle", "number", false, nullptr, nullptr, nullptr, nullptr, "太陽の角半径の tan。0.001..0.5 にクランプされる。既定 0.05(誇張値)。実際の太陽は 0.0044。"),
            P("maxPenumbraTexels", "number", false, nullptr, nullptr, nullptr, nullptr, "ペナンブラ幅の上限(シャドウマップのテクセル数)。1..64 にクランプ。大きいほど柔らかく重い。"),
            P("blockerSearchTexels", "number", false, nullptr, nullptr, nullptr, nullptr, "ブロッカー探索の半径(シャドウマップのテクセル数)。1..64 にクランプ。小さすぎると遠くの影が硬いまま。"),
            P("temporalDither", "bool", false, nullptr, nullptr, nullptr, nullptr, "サンプル位置をフレームごとに回してバンディングを散らす。★TAA 有効時のみ効く(無効だとチラつくだけなのでエンジンが自動で切る)。"),
        })});
    o.push_back({"set_sprite_shader", M("エンティティの Sprite2D::shaderPath を設定/解除する(Inspector の Sprite2D「Shader」欄と同じ操作)。",
        "set sprite shader Sprite2Dカスタムシェーダー割当",
        "material", "", "", McpEffect::WriteScene, "any", 10000, true, false,
        "none", "dx12_set_sprite_shader", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("shaderPath", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets/shaders 相対パス。例: Dissolve.hlsl。省略/空文字で既定 Sprite シェーダーに戻す。"),
            P("alphaBlend", "bool", false, nullptr, nullptr, nullptr, nullptr, "true でシェーダーの alpha 出力を SrcAlpha/InvSrcAlpha ブレンドに使う(DepthWrite OFF)。省略時は既存値を維持、既定は false(不透明固定)。"),
        })});
    o.push_back({"set_ssao", M("SSAO のフィールドを指定分だけ更新する(未指定は現状維持)。radius=ワールド空間半径, bias=自己遮蔽バイアス, intensity=遮蔽の強さ, power=コントラスト(pow指数), sampleCount=8か16, blur=4x4ボックスブラーの有無。",
        "set ssao SSAO設定変更",
        "render", "render_setting", "ssao", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_ssao", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("radius", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("bias", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("intensity", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("power", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("sampleCount", "int", false, nullptr, nullptr, nullptr, nullptr, "8 か 16。"),
            P("blur", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"set_ssgi", M("SSGI(スクリーン空間GI) のフィールドを指定分だけ更新する(未指定は現状維持)。前フレームのシーンカラーを間接光のソースにして、IBL の拡散(irradiance)を置き換える。",
        "set ssgi SSGI設定変更",
        "render", "render_setting", "ssgi", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_ssgi", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("intensity", "number", false, nullptr, nullptr, nullptr, nullptr, "間接拡散の強さ。既定 0.8"),
            P("radius", "number", false, nullptr, nullptr, nullptr, nullptr, "レイの最大到達距離(m)"),
            P("thickness", "number", false, nullptr, nullptr, nullptr, nullptr, "ヒットとみなす深度差の上限(m)"),
            P("rayCount", "int", false, nullptr, nullptr, nullptr, nullptr, "1..4"),
            P("stepCount", "int", false, nullptr, nullptr, nullptr, nullptr, "4..24"),
            P("clampValue", "number", false, nullptr, nullptr, nullptr, nullptr, "積分結果の輝度クランプ(firefly/発散対策)"),
            P("feedback", "number", false, nullptr, nullptr, nullptr, nullptr, "時間蓄積の履歴比率 0.8..0.98"),
            P("iblFallback", "bool", false, nullptr, nullptr, nullptr, nullptr, "画面外へ抜けたレイに irradiance キューブを積む"),
        })});
}

void FillManifestChunk12(ManifestRows& o)
{
    o.push_back({"set_ssr", M("SSR(スクリーン空間反射) のフィールドを指定分だけ更新する(未指定は現状維持)。深度プリパスの G-Buffer(法線/ラフネス) と前フレームのシーンカラーをレイマーチして、IBL の鏡面反射を置き換える。",
        "set ssr SSR設定変更",
        "render", "render_setting", "ssr", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_ssr", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("intensity", "number", false, nullptr, nullptr, nullptr, nullptr, "0..1。confidence への乗算"),
            P("maxDistance", "number", false, nullptr, nullptr, nullptr, nullptr, "レイの最大到達距離(m)"),
            P("thickness", "number", false, nullptr, nullptr, nullptr, nullptr, "ヒットとみなす深度差の上限(m)"),
            P("maxSteps", "int", false, nullptr, nullptr, nullptr, nullptr, "16..128"),
            P("stride", "number", false, nullptr, nullptr, nullptr, nullptr, "DDA の 1 ステップのピクセル数 1..8"),
            P("roughnessCutoff", "number", false, nullptr, nullptr, nullptr, nullptr, "これを超えるラフネスは IBL に任せる"),
            P("edgeFade", "number", false, nullptr, nullptr, nullptr, nullptr, "画面端フェード幅(NDC 比 0..0.5)"),
            P("bias", "number", false, nullptr, nullptr, nullptr, nullptr, "レイ始点の押し出し(m)"),
        })});
    o.push_back({"set_sun", M("シーンの太陽(最初の DirectionalLight)の向き・色・強度・環境光を【絶対値で】設定する(冪等)。",
        "set sun 太陽(平行光)を設定",
        "lighting", "", "", McpEffect::WriteSetting, "any", 8000, true, false,
        "none", "dx12_set_sun", {
            P("timeOfDay", "number", false, nullptr, nullptr, nullptr, nullptr, "0..24 の時刻。向き/色/強度/環境光をまとめて決める。物理大気が ON のときは従来の曲線ではなく大気の時刻(atmosphere.timeOfDay)を設定する。"),
            P("azimuth", "number", false, nullptr, nullptr, nullptr, nullptr, "方位角(度)。+Z が 0°、+X が 90°。物理大気が ON のときは atmosphere.sunMode=1(太陽の向きを直接指定)へ切り替わる。"),
            P("elevation", "number", false, nullptr, nullptr, nullptr, nullptr, "高度角(度)。0=地平線、90=真上(-89..89)。"),
            P("color", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[r,g,b] 0..1。kelvin より優先。"),
            P("kelvin", "number", false, nullptr, nullptr, nullptr, nullptr, "色温度 K(1000..40000)。2900=電球色 / 5600=昼白色 / 7800=曇天。"),
            P("intensity", "number", false, nullptr, nullptr, nullptr, nullptr, "光の強さ(0..100)。"),
            P("ambient", "number", false, nullptr, nullptr, nullptr, nullptr, "この光が供給する環境光(影部分の明るさ。0..5)。"),
        })});
    o.push_back({"set_taa", M("TAA のフィールドを指定分だけ更新する(未指定は現状維持)。速度バッファ(モーションベクター)と前フレームの履歴を使うサブピクセル AA で、FXAA と違って動いている物もぼけない。",
        "set taa TAA設定変更",
        "render", "render_setting", "taa", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_taa", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("sampleCount", "int", false, nullptr, nullptr, nullptr, nullptr, "ハルトン列の周期。4=シャープ / 8=標準 / 16=滑らか。"),
            P("feedbackMin", "number", false, nullptr, nullptr, nullptr, nullptr, "現フレームと食い違うピクセルで使う履歴の比率。既定 0.88。"),
            P("feedbackMax", "number", false, nullptr, nullptr, nullptr, nullptr, "安定しているピクセルで使う履歴の比率。既定 0.97。高いほど滑らかだがゴーストしやすい。"),
            P("varianceGamma", "number", false, nullptr, nullptr, nullptr, nullptr, "近傍色の許容幅 μ±γσ。既定 1.0。下げるとゴーストが減りチラつきが増える。"),
            P("jitterScale", "number", false, nullptr, nullptr, nullptr, nullptr, "ジッタ量の倍率。1.0 = ±0.5px。ブラーが強すぎるなら下げる。"),
            P("debugVelocity", "bool", false, nullptr, nullptr, nullptr, nullptr, "速度バッファを画面に可視化する(検証用)。静止時に全面が均一なグレーになるのが正常で、縞々に揺れていたらジッタ除去のバグ。カメラを右へパンすると赤寄り、左で緑寄り。"),
        })});
    o.push_back({"set_texture", M("エンティティの MeshRenderer にテクスチャを割り当てる(Inspector のアセットブラウザ D&D と同じ操作)。",
        "set texture テクスチャ上書き割当",
        "material", "", "", McpEffect::WriteScene, "any", 10000, true, false,
        "none", "dx12_set_texture", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス(例: textures/rust.png)。空文字で override 解除。"),
            P("slot", "enum", false, "albedo|normal|metalRoughness|emissive", nullptr, nullptr, nullptr, "テクスチャスロット。省略で albedo。emissive は自己発光(dx12_set_pbr の emissiveIntensity と併用)。"),
            P("submesh", "int", false, nullptr, nullptr, nullptr, nullptr, "サブメッシュ index。省略で 0。"),
        })});
    o.push_back({"set_transform", M("エンティティの Transform を設定する。指定したフィールドだけ更新。回転は rotation(Euler 度) か quaternion([x,y,z,w]) のどちらか。",
        "set transform Transform 設定",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_set_transform", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z]"),
            P("rotation", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] Euler 度。quaternion と併用しない。"),
            P("quaternion", "vec4", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z,w] クォータニオン。rotation と併用しない。"),
            P("scale", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z]"),
        })});
    o.push_back({"set_volumetric_fog", M("ボリュメトリックフォグのフィールドを指定分だけ更新する(未指定は現状維持)。視錐台に沿った 3D テクスチャ(160x90x64)へ散乱を焼いてから画面へ合成する方式で、空気そのものが光る=光の筋(ゴッドレイ)が立体的に見える。",
        "set volumetric fog ボリュメトリックフォグ設定変更",
        "render", "render_setting", "volumetric_fog", McpEffect::WriteSetting, "any", 10000, true, false,
        "none", "dx12_set_volumetric_fog", {
            P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("density", "number", false, nullptr, nullptr, nullptr, nullptr, "消散係数 σ_t(1/m 相当)。既定 0.02。0.05 で濃い霧、0.2 でほぼ視界ゼロ。"),
            P("albedo", "vec3", false, nullptr, nullptr, nullptr, nullptr, "散乱アルベド [r,g,b]。σ_s = density * albedo。"),
            P("anisotropy", "number", false, nullptr, nullptr, nullptr, nullptr, "Henyey-Greenstein の g(-0.9..0.9)。既定 0.3。0=等方 / 0.6-0.8 で太陽方向に強いシャフト。負にすると後方散乱。"),
            P("heightFalloff", "number", false, nullptr, nullptr, nullptr, nullptr, "高さ方向の指数減衰(1/m)。既定 0.1。0 で高さ無依存。"),
            P("heightRef", "number", false, nullptr, nullptr, nullptr, nullptr, "高さ減衰の基準高さ(world Y)。既定 0。"),
            P("distance", "number", false, nullptr, nullptr, nullptr, nullptr, "froxel ボリュームの到達距離(m)。既定 150。ここから先は解析フォグへ引き継ぐ。"),
            P("depthDistribution", "number", false, nullptr, nullptr, nullptr, nullptr, "Z 分布の冪 k(1..4)。z = distance * w^k。既定 2。1=線形 / 大きいほど手前が細かい。"),
            P("ambient", "vec3", false, nullptr, nullptr, nullptr, nullptr, "環境散乱(等方) [r,g,b]。影の中の霧の明るさ。"),
            P("sunIntensity", "number", false, nullptr, nullptr, nullptr, nullptr, "太陽の散乱寄与スケール。既定 1。"),
            P("lightScattering", "bool", false, nullptr, nullptr, nullptr, nullptr, "点光源/スポットも散乱させるか(クラスタライトリストを引く)。既定 true。"),
            P("temporal", "bool", false, nullptr, nullptr, nullptr, nullptr, "時間再投影。既定 true。false にするとサブfroxelジッタも自動で切れる。"),
            P("temporalBlend", "number", false, nullptr, nullptr, nullptr, nullptr, "現フレームの比率(0.01..1)。既定 0.08。小さいほど滑らかだがゴーストが増える。"),
            P("extendBeyondRange", "bool", false, nullptr, nullptr, nullptr, nullptr, "distance より遠方を解析的な指数フォグで延長する。既定 true。切ると遠景に『フォグが止まる帯』が出る。"),
            P("debugMode", "int", false, nullptr, nullptr, nullptr, nullptr, "0=オフ / 1=散乱だけ / 2=透過率だけ / 3=froxel スライスの縞。保存されない検証用。"),
        })});
    o.push_back({"snap_to_ground", M("エンティティを直下の床/他メッシュの天面に置く(Editor 中でも動く)。既定は三角形単位の精密レイキャストで真下の【実際の面】に乗せる(斜面・階段・地形の起伏に追従)。",
        "snap to ground 接地(下の面に置く)",
        "entity", "", "", McpEffect::WriteScene, "any", 8000, true, false,
        "none", "dx12_snap_to_ground", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("offset", "number", false, nullptr, nullptr, nullptr, nullptr, "接地面からの追加オフセット(m)。既定 0。"),
            P("precise", "bool", false, nullptr, nullptr, nullptr, nullptr, "三角形単位の精密レイキャストで接地面を決める。既定 true。false にすると旧来の AABB 天面判定だけになる(地形の上で山頂の高さに吸い付く)。"),
        })});
    o.push_back({"spawn_model", M("モデル(.gltf/.glb/.fbx/.obj)を assets 相対パスから生成する。GPU ロードを伴いフレーム境界で実処理されるが、Node が完了を待って【本物の {entityId, name, sceneGeneration} を同期で返す】。",
        "spawn model モデル生成",
        "entity", "", "", McpEffect::WriteScene, "editor", 45000, false, true,
        "none", "dx12_spawn_model", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: models/player.glb"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z]。省略時 [0,0,0]。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名。省略時はファイル名(拡張子なし)。"),
        })});
    o.push_back({"spawn_prefab", M("プレハブ(.prefab)を assets 相対パスから生成する。フレーム境界で実処理され、Node が完了を待って【本物の {entityId, rootEntityId, entityIds:[...], name, sceneGeneration} を同期で返す】。",
        "spawn prefab プレハブ生成",
        "entity", "", "", McpEffect::WriteScene, "editor", 45000, false, true,
        "none", "dx12_spawn_prefab", {
            P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。例: prefabs/enemy.prefab"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z]。省略時 [0,0,0]。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "ルートエンティティ名。省略時はプレハブ名。"),
        })});
    o.push_back({"step_frames", M("N フレーム経過してから応答する同期バリア。",
        "step frames Nフレーム進める",
        "play", "", "", McpEffect::Runtime, "any", 30000, false, true,
        "none", "dx12_step_frames", {
            P("frames", "int", false, nullptr, nullptr, nullptr, nullptr, "進めるフレーム数(既定 1, 最大 600)。"),
            P("deterministic", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で dt を固定し、再現するステップにする。"),
            P("dt", "number", false, nullptr, nullptr, nullptr, nullptr, "固定 dt(秒)。既定 1/60 = 0.016667。deterministic:true のときだけ有効。"),
            P("hold", "bool", false, nullptr, nullptr, nullptr, nullptr, "進めた後に時間を止めるか(既定 true)。deterministic:true のときだけ有効。false で従来どおり走らせ続ける。"),
            P("n", "int", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"stop", M("Playing → Editor へ切り替える(再生前のスナップショットに復元)。フレーム境界で実処理され {mode:'Editor', sceneGeneration} を同期で返す。",
        "stop 再生停止",
        "play", "", "", McpEffect::Runtime, "any", 20000, false, true,
        "none", "dx12_stop", {
        })});
    o.push_back({"terrain_autopaint", M("傾斜と標高から 4 層のスプラット重みを全面焼き直す(草→土→岩→雪)。★冪等: 何度呼んでも同じ結果になり、手で塗った内容(dx12_terrain_paint)は上書きされて消える。",
        "terrain autopaint 地形レイヤーを自動で焼き直す",
        "terrain", "terrain", "autopaint", McpEffect::WriteFile, "editor", 30000, true, false,
        "none", "dx12_terrain_autopaint", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("rockSlopeStart", "number", false, nullptr, nullptr, nullptr, nullptr, "岩が混ざり始める傾斜 0..1(0=平ら, 1=垂直)。"),
            P("rockSlopeEnd", "number", false, nullptr, nullptr, nullptr, nullptr, "岩だけになる傾斜 0..1。Start より大きくする。"),
            P("dirtSlopeStart", "number", false, nullptr, nullptr, nullptr, nullptr, "土が混ざり始める傾斜 0..1。岩より緩い側。"),
            P("dirtSlopeEnd", "number", false, nullptr, nullptr, nullptr, nullptr, "土だけになる傾斜 0..1。"),
            P("snowHeightStart", "number", false, nullptr, nullptr, nullptr, nullptr, "雪が積もり始める標高(ワールド Y, m)。指定すると自動雪線が切れる。"),
            P("snowHeightEnd", "number", false, nullptr, nullptr, nullptr, nullptr, "完全に雪になる標高(ワールド Y, m)。Start と対で渡す。"),
            P("noiseStrength", "number", false, nullptr, nullptr, nullptr, nullptr, "境界を乱すノイズ量 0..1。0 だと帯が定規で引いたようになる。"),
            P("falloff", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("layer", "int", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("point", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("points", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("radius", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("strength", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("worldPos", "any", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
}

void FillManifestChunk13(ManifestRows& o)
{
    o.push_back({"terrain_create", M("ハイトフィールド地形を作る(静的コライダー付き)。★冪等: 同じ name の地形が既にあれば作り直さず設定だけ更新する(resolution か worldSize を変えたときだけ高さ配列がリセットされ heightsReset:true が返る)。",
        "terrain create 地形を作る/設定を更新",
        "terrain", "terrain", "create", McpEffect::WriteFile, "editor", 45000, true, true,
        "none", "dx12_terrain_create", {
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(既定 \"Terrain\")。同名があれば設定更新になる。"),
            P("resolution", "int", false, nullptr, nullptr, nullptr, nullptr, "1 辺のサンプル数(既定 128、16..512。内部で 4 の倍数へ丸め)。"),
            P("worldSize", "number", false, nullptr, nullptr, nullptr, nullptr, "1 辺のワールド長 m(既定 200)。セル幅 = worldSize/(resolution-1)。"),
            P("maxHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "ブラシの高さクランプ ±この値(既定 200)。"),
            P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] 地形の原点(既定 [0,0,0])。地形は XZ グリッドなので回転/スケールは効かない。"),
            P("uvScale", "number", false, nullptr, nullptr, nullptr, nullptr, "地形全体での UV 繰り返し数(既定 24)。タイリングテクスチャの密度。"),
            P("color", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[r,g,b] 0..1 頂点色(マテリアル未割当時の見た目)。"),
        })});
    o.push_back({"terrain_erode", M("熱浸食(安息角 talusDeg を超えた斜面の土砂を隣へ落とす)を掛ける。生成直後の CG くさい斜面が一気に自然になる。",
        "terrain erode 地形を浸食させる",
        "terrain", "terrain", "erode", McpEffect::WriteFile, "editor", 60000, false, false,
        "none", "dx12_terrain_erode", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("iterations", "int", false, nullptr, nullptr, nullptr, nullptr, "反復回数(既定 16、1..200)。多いほど崩れて滑らかになる。"),
            P("talusDeg", "number", false, nullptr, nullptr, nullptr, nullptr, "安息角 度(既定 34)。小さいほどよく崩れる。"),
            P("region", "vec4", false, nullptr, nullptr, nullptr, nullptr, "[minX,minZ,maxX,maxZ] ワールド XZ の矩形。省略で地形全面。"),
        })});
    o.push_back({"terrain_generate", M("fBm ノイズで地形の高さを丸ごと作り直す。preset は hills(なだらかな丘) / canyon(峡谷) / mountains(険しい山脈)。",
        "terrain generate 地形を一発生成(fBm)",
        "terrain", "terrain", "generate", McpEffect::WriteFile, "editor", 30000, true, false,
        "none", "dx12_terrain_generate", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("preset", "enum", false, "hills|canyon|mountains", nullptr, nullptr, nullptr, "生成プリセット(既定 hills)。"),
            P("seed", "int", false, nullptr, nullptr, nullptr, nullptr, "乱数シード(既定 1337)。変えると同じ preset でも別の地形になる。"),
            P("frequency", "number", false, nullptr, nullptr, nullptr, nullptr, "空間周波数。小さいほど大きな起伏(0.0001..1)。"),
            P("octaves", "int", false, nullptr, nullptr, nullptr, nullptr, "重ねるノイズの段数(1..8)。多いほどディテールが増える。"),
            P("amplitude", "number", false, nullptr, nullptr, nullptr, nullptr, "高さの振幅 m。"),
            P("ridged", "number", false, nullptr, nullptr, nullptr, nullptr, "0..1。1 に近いほど鋭い尾根(山脈らしくなる)。"),
            P("baseHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "全体のかさ上げ m。"),
            P("edgeFalloff", "number", false, nullptr, nullptr, nullptr, nullptr, "0..1。>0 で外周へ向かって高さを落とす(島にする / 縁の崖を防ぐ)。"),
            P("valleyDepth", "number", false, nullptr, nullptr, nullptr, nullptr, ">0 で低い所をさらに下げる(峡谷になる)。"),
        })});
    o.push_back({"terrain_paint", M("地形のテクスチャレイヤー(4 層スプラット)の重みを円ブラシで塗る。layer は 0..3 で .terrainlayers の並び順(既定は 0=草 / 1=土 / 2=岩 / 3=雪)。",
        "terrain paint 地形レイヤーを塗る",
        "terrain", "terrain", "paint", McpEffect::WriteFile, "editor", 8000, false, false,
        "none", "dx12_terrain_paint", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("layer", "int", false, nullptr, nullptr, nullptr, nullptr, "塗るレイヤー index 0..3(既定 0)。.terrainlayers の並び順。"),
            P("point", "vec2", false, nullptr, nullptr, nullptr, nullptr, "[x,z] ワールド座標の 1 点。"),
            P("points", "array", false, nullptr, nullptr, nullptr, nullptr, "[[x,z],...] 連続ストローク(最大 512 点)。[x,y,z] でも可(y は無視)。"),
            P("worldPos", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] ワールド座標(y は無視)。dx12_pick の worldPos をそのまま渡せる。"),
            P("radius", "number", false, nullptr, nullptr, nullptr, nullptr, "ブラシ半径 m(既定 12、0.01..地形の worldSize)。"),
            P("strength", "number", false, nullptr, nullptr, nullptr, nullptr, "1 ストロークぶんの塗り量 0..1(既定 0.7)。1 なら一発でそのレイヤー 100%。"),
            P("falloff", "number", false, nullptr, nullptr, nullptr, nullptr, "縁のぼかし 0..1(既定 0.5)。0=硬い縁 / 1=とろけるように滑らか。"),
            P("dirtSlopeEnd", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("dirtSlopeStart", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("noiseStrength", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("rockSlopeEnd", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("rockSlopeStart", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("snowHeightEnd", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
            P("snowHeightStart", "number", false, nullptr, nullptr, nullptr, nullptr, nullptr),
        })});
    o.push_back({"terrain_sample", M("地形の高さ・法線・傾きを座標で問い合わせる(読み取り専用)。points:[[x,z],...] を渡すと各点の {x,z,height,worldY,normal,slopeDeg,inside} が返る。",
        "terrain sample 地形の高さ/法線を問い合わせ",
        "terrain", "terrain", "sample", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_terrain_sample", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("points", "array", false, nullptr, nullptr, nullptr, nullptr, "[[x,z],...] ワールド座標(最大 512 点)。[x,y,z] でも可(y は無視)。"),
        })});
    o.push_back({"terrain_sculpt", M("地形をブラシで彫る。point:[x,z] で 1 点、points:[[x,z],...] で連続ストローク(稜線・道・堀を一気に引ける。最大 512 点)。",
        "terrain sculpt 地形をブラシで彫る",
        "terrain", "terrain", "sculpt", McpEffect::WriteFile, "editor", 8000, false, false,
        "none", "dx12_terrain_sculpt", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("brush", "enum", false, "raise|lower|smooth|flatten|noise", nullptr, nullptr, nullptr, "ブラシ種別(既定 raise)。浸食は dx12_terrain_erode。"),
            P("point", "vec2", false, nullptr, nullptr, nullptr, nullptr, "[x,z] ワールド座標の 1 点。"),
            P("points", "array", false, nullptr, nullptr, nullptr, nullptr, "[[x,z],...] 連続ストローク(最大 512 点)。[x,y,z] でも可(y は無視)。"),
            P("worldPos", "vec3", false, nullptr, nullptr, nullptr, nullptr, "[x,y,z] ワールド座標(y は無視)。dx12_pick の worldPos をそのまま渡せる。"),
            P("radius", "number", false, nullptr, nullptr, nullptr, nullptr, "ブラシ半径 m(既定 12)。"),
            P("strength", "number", false, nullptr, nullptr, nullptr, nullptr, "1 ストロークぶんの適用量(既定 5)。"),
            P("falloff", "number", false, nullptr, nullptr, nullptr, nullptr, "縁のぼかし 0..1(既定 0.5)。0=硬い縁 / 1=とろけるように滑らか。"),
            P("flattenHeight", "number", false, nullptr, nullptr, nullptr, nullptr, "brush:flatten の目標高さ(ワールド Y)。省略時は最初の点の現在高さ。"),
            P("mirrorX", "bool", false, nullptr, nullptr, nullptr, nullptr, "X ミラー(x を反転した位置にも同じ筆を置く)。"),
            P("mirrorZ", "bool", false, nullptr, nullptr, nullptr, nullptr, "Z ミラー。"),
            P("noiseFrequency", "number", false, nullptr, nullptr, nullptr, nullptr, "brush:noise の周波数(既定 0.03)。"),
            P("noiseOctaves", "int", false, nullptr, nullptr, nullptr, nullptr, "brush:noise のオクターブ(1..8)。"),
            P("noiseRidged", "number", false, nullptr, nullptr, nullptr, nullptr, "brush:noise の尾根っぽさ 0..1。"),
            P("seed", "int", false, nullptr, nullptr, nullptr, nullptr, "brush:noise のシード。"),
        })});
    o.push_back({"terrain_set_layers", M("地形へ .terrainlayers(4 層の PBR 素材セット)を割り当てる/外す。★これが『地形にテクスチャを載せる唯一の MCP 経路』。",
        "terrain set layers 地形にテクスチャレイヤーを割り当てる",
        "terrain", "terrain", "set_layers", McpEffect::WriteFile, "editor", 30000, true, false,
        "none", "dx12_terrain_set_layers", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("layerSetPath", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対の .terrainlayers(例: terrain/alpine.terrainlayers)。空文字 \"\" で割当解除。存在しなければ NOT_FOUND。"),
            P("splatResolution", "int", false, nullptr, nullptr, nullptr, nullptr, "スプラットの一辺(32..2048、既定 512。2 の冪へ正規化される)。初回作成時のみ効く。"),
            P("autopaint", "bool", false, nullptr, nullptr, nullptr, nullptr, "スプラットを新規作成したとき傾斜/標高から自動で塗るか(既定 true)。false だとレイヤー 0 一色。"),
            P("uvScale", "number", false, nullptr, nullptr, nullptr, nullptr, "レイヤーテクスチャのタイリング倍率(0.01..1000)。大きいほど細かく繰り返す。"),
            P("heightBlendDepth", "number", false, nullptr, nullptr, nullptr, nullptr, "ハイトブレンドの食い込み深さ 0.01..1。大きいほど層の境界が『石の隙間に砂が入る』ような噛み合いになる。"),
            P("triplanarSharpness", "number", false, nullptr, nullptr, nullptr, nullptr, "三平面投影のブレンド鋭さ 1..16。大きいほど面の切り替わりが硬い。"),
            P("normalStrength", "number", false, nullptr, nullptr, nullptr, nullptr, "レイヤー法線マップの強さ 0..2。0 で法線マップ無効。"),
            P("macroScale", "number", false, nullptr, nullptr, nullptr, nullptr, "マクロバリエーションの周期(m) 10..400。遠景のタイリング感を崩す模様の大きさ。"),
            P("macroStrength", "number", false, nullptr, nullptr, nullptr, nullptr, "マクロバリエーションの強さ 0..1。0 で無効。"),
            P("distTilingStart", "number", false, nullptr, nullptr, nullptr, nullptr, "距離タイリング低減が始まる距離(m) 5..200。"),
            P("distTilingFarScale", "number", false, nullptr, nullptr, nullptr, nullptr, "遠景でのタイリング倍率 2..16。大きいほど遠くの繰り返しが目立たなくなる。"),
            P("pomHeightScale", "number", false, nullptr, nullptr, nullptr, nullptr, "視差オクルージョンマッピングの高さ 0..0.3。0 で凹凸なし。上げすぎると輪郭が溶ける。"),
            P("pomFadeStart", "number", false, nullptr, nullptr, nullptr, nullptr, "POM のフェード開始距離(m) 0..40。"),
            P("pomFadeEnd", "number", false, nullptr, nullptr, nullptr, nullptr, "POM が完全に消える距離(m) 1..120。Start より大きくする。"),
            P("triplanar", "bool", false, nullptr, nullptr, nullptr, nullptr, "三平面投影を使うか(急斜面の引き伸ばし対策)。terrainMatFlags bit0。"),
            P("pom", "bool", false, nullptr, nullptr, nullptr, nullptr, "視差オクルージョンマッピングを使うか。terrainMatFlags bit1。重い。"),
            P("macro", "bool", false, nullptr, nullptr, nullptr, nullptr, "マクロバリエーションを使うか。terrainMatFlags bit2。"),
            P("distTiling", "bool", false, nullptr, nullptr, nullptr, nullptr, "距離タイリング低減を使うか。terrainMatFlags bit3。"),
        })});
    o.push_back({"terrain_splat_info", M("地形のスプラット(4 層の重みテクスチャ)の要約を返す【読み取り専用】ツール。★dx12_terrain_paint / dx12_terrain_autopaint の結果を『絵を見ずに数値で』検証するのに使う。",
        "terrain splat info 地形スプラットの要約を読む",
        "terrain", "terrain", "splat_info", McpEffect::Read, "any", 8000, true, false,
        "none", "dx12_terrain_splat_info", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("gridSize", "int", false, nullptr, nullptr, nullptr, nullptr, "支配レイヤーの粗いグリッドの一辺(0..32、既定 8)。0 を渡すと grid を返さない(coverage だけ欲しいとき)。"),
            P("point", "vec2", false, nullptr, nullptr, nullptr, nullptr, "[x,z] ワールド座標 1 点の重みを見る。[x,y,z] でも可(y は無視)。"),
            P("points", "array", false, nullptr, nullptr, nullptr, nullptr, "[[x,z],...] 複数点(最大 256)。point と併用すると両方が samples に入る(読み取りなので二重適用の心配は無い)。"),
        })});
    o.push_back({"transaction_begin", M("以降の MCP の編集(生成・削除・複製・Transform・コンポーネント・親子・名前・色/PBR/テクスチャ・Lua プロパティ・地形/スカルプト)を 1 つの Undo エントリ「AI: <label>」へまとめ始める。",
        "transaction begin トランザクション開始",
        "undo", "", "", McpEffect::WriteScene, "editor", 10000, false, false,
        "none", "dx12_transaction_begin", {
            P("label", "string", false, nullptr, nullptr, nullptr, nullptr, "Undo 履歴に出る名前(「AI: <label>」)。省略で transaction。"),
        })});
    o.push_back({"transaction_commit", M("begin 以降の MCP の編集を 1 エントリとして Undo に積んで閉じる(遅延応答)。返り値 {committed, label, calls, pushed, entryName, humanEditsDuringTransaction, top}。",
        "transaction commit トランザクション確定",
        "undo", "", "", McpEffect::WriteScene, "any", 45000, false, true,
        "none", "dx12_transaction_commit", {
        })});
    o.push_back({"transaction_rollback", M("begin 以降の MCP の編集を全部逆順に戻して閉じる(遅延応答。消した物も guid ごと復元される)。返り値 {rolledBack, label, calls, humanEditsDuringTransaction, top, sceneGeneration}。",
        "transaction rollback トランザクション巻き戻し",
        "undo", "", "", McpEffect::WriteScene, "editor", 45000, false, true,
        "none", "dx12_transaction_rollback", {
        })});
    o.push_back({"transaction_status", M("開いているか・中身・放置時間・直前に閉じた理由を返す。",
        "transaction status トランザクションの状態",
        "undo", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_transaction_status", {
        })});
}

void FillManifestChunk14(ManifestRows& o)
{
    o.push_back({"ui_click", M("ゲーム内 UI（UIButton / UIToggle / UISlider）を合成ポインタで押す。★テストプレイで AI がメニューを操作する口。",
        "ui click ゲーム内UIを押す",
        "ui", "", "", McpEffect::Runtime, "any", 10000, false, true,
        "none", "dx12_ui_click", {
            P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。id の代わりに使える。Stop 後など id が変わる場面で安定。"),
            P("x", "number", false, nullptr, nullptr, nullptr, nullptr, "ビューポート基準 0..1 の横位置。name/entity の代わりに使う"),
            P("y", "number", false, nullptr, nullptr, nullptr, nullptr, "ビューポート基準 0..1 の縦位置。name/entity の代わりに使う"),
            P("move", "bool", false, nullptr, nullptr, nullptr, nullptr, "true で押さずにカーソルを動かすだけ(ホバーの確認用)。既定 false"),
        })});
    o.push_back({"ui_screenshot", M("エディタウィンドウ全体(ImGui パネル込み)を PNG で返す。★dx12_screenshot(シーンRT)には写らないゲーム内 UI プレビュー・UIエディタ・インスペクタが写る = AI が組んだ UI の見た目を目で確認して直すのに使う。",
        "ui screenshot UIスクリーンショット",
        "ui", "", "", McpEffect::Read, "any", 10000, true, true,
        "none", "dx12_ui_screenshot", {
        })});
    o.push_back({"ui_tree", M("ゲーム内 UI のツリー構造を丸ごと JSON で返す(キャンバスごと)。",
        "ui tree UIツリー取得",
        "ui", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_ui_tree", {
        })});
    o.push_back({"undo", M("エディタの Undo スタックを 1 つ戻す(フレーム境界で適用される遅延応答)。返り値 {undone, wasAi, onlyAi, undoable, willUndo, next:{undo, redo}}(互換で queuedUndo も残る)。",
        "undo Undo",
        "undo", "", "", McpEffect::WriteScene, "editor", 45000, false, true,
        "none", "dx12_undo", {
            P("onlyAi", "bool", false, nullptr, nullptr, nullptr, nullptr, "true(既定)= 一番上が AI の編集のときだけ戻す / false = 人の編集でも戻す。"),
        })});
    o.push_back({"validate_layout", M("置いた物の【見れば分かるが AI は見ない】破綻を数値で拾う。",
        "validate layout 配置検査",
        "quality", "", "", McpEffect::Read, "editor", 10000, true, false,
        "none", "dx12_validate_layout", {
            P("fix", "enum", false, "none|safe|all", nullptr, nullptr, nullptr, "none(既定)=検査のみ / safe=安全な修正だけ / all=全部。"),
            P("tolerance", "number", false, nullptr, nullptr, nullptr, nullptr, "同一平面とみなす距離(m)。既定 0.001(1mm)。"),
        })});
    o.push_back({"validate_scene", M("シーン JSON の参照グラフをヘッドレスで検証する(CLI `--validate` と同じロジックをエンジン自身の子プロセスとして実行)。",
        "validate scene シーン検証",
        "quality", "", "", McpEffect::Read, "any", 10000, true, false,
        "none", "dx12_validate_scene", {
            P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "assets 相対パス。省略時は現在開いているシーン。"),
        })});
}

void FillGeneratedManifestRows(ManifestRows& o)
{
    FillManifestChunk0(o);
    FillManifestChunk1(o);
    FillManifestChunk2(o);
    FillManifestChunk3(o);
    FillManifestChunk4(o);
    FillManifestChunk5(o);
    FillManifestChunk6(o);
    FillManifestChunk7(o);
    FillManifestChunk8(o);
    FillManifestChunk9(o);
    FillManifestChunk10(o);
    FillManifestChunk11(o);
    FillManifestChunk12(o);
    FillManifestChunk13(o);
    FillManifestChunk14(o);
}
