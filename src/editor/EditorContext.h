#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <functional>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "core/mcp/McpDeferred.h"
#include "editor/UndoSystem.h"
#include "editor/EditorIcons.h"
#include "renderer/DrawItem.h"   // ピッキングのブロードフェーズ候補（Application が毎フレーム構築）

namespace dx12e
{

class Camera;

enum class GizmoMode { Translate, Rotate, Scale };

// ビューポート上のマウス入力の最小スナップショット。
// SceneViewPanel が「UI 編集・3D ピッキングより前」に EditorContext::viewportToolHandlers を
// 順に呼び、true を返したハンドラがそのクリックを消費する（以降のピック/UI編集は行わない）。
// 地形ブラシ・ライトのハンドル操作など、後から生えるビューポートツールの受け口。
struct ViewportInput
{
    f32 mouseX = 0.0f, mouseY = 0.0f;                    // スクリーン座標（ImGui 座標）
    f32 vpX = 0.0f, vpY = 0.0f, vpW = 0.0f, vpH = 0.0f;  // ビューポート矩形
    Camera* camera = nullptr;
    DirectX::XMFLOAT3 rayOrigin{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rayDir{0.0f, 0.0f, 1.0f};          // 正規化済み
    bool inViewport = false;
    bool pressed    = false;   // 今フレーム左ボタンを押した
    bool dragging   = false;   // 左ボタンを押したまま動かしている
    bool released   = false;   // 今フレーム左ボタンを離した
};

// ツールバーのPlayドロップダウンで選ぶマルチプレイのテストロール(フェーズ⑨)。
// EnterPlayModeがこれを見てnet:host()/net:join()相当を自動実行する(Lua無しでの手早い動作確認用)。
enum class NetTestRole { Offline, Host, Client };

struct PendingSpawnRequest
{
    std::string modelPath;
    DirectX::XMFLOAT3 position{};
    std::string name;   // 空ならデフォルト名。MCP の create_entity から任意名を付けるのに使う。
    McpDeferred mcp;    // MCP 由来なら相関情報。生成後に本物の entityId を送り返す。client=0 で無効。
    // UI 要素(__ui_*__)の親の明示指定(MCP 用)。null なら従来どおり
    // 「選択中の UI ツリー → 最初の Canvas → 自動生成」の順で解決する。
    entt::entity parent = entt::null;

    // UI エディタのキャンバスへ .prefab をドロップした時だけ立つ。
    // 生成後に root の UIRect を uiCanvasPos（キャンバス基準解像度の px 座標）へ移す。
    // position(3D ワールド座標)とは別枠にしてあるのは、UI は Transform を使わへんため。
    bool placeInUiCanvas = false;
    DirectX::XMFLOAT2 uiCanvasPos{0.0f, 0.0f};
};

// MCP 由来のエンティティ削除要求。フレーム境界でサブツリー削除後に deletedCount を送り返す。
// エディタ UI からの削除は従来どおり pendingDeletions(下) を使い、応答は送らない。
struct McpPendingDelete
{
    entt::entity entity = entt::null;
    McpDeferred  mcp;
};

// MCP 由来の「手続き的メッシュ」生成要求（地形 / スカルプト素体 / モデルの編集可能化）。
// Scene::SpawnTerrain / SpawnSculpt は GPU へメッシュを載せるので記録中の cmdList が要る＝
// MCP の受信時点（フレーム先頭）では実行できない。pendingSpawns と同じくフレーム境界で処理し、
// 本物の entityId を遅延応答で返す。
struct McpPendingProcCreate
{
    enum class Kind { Terrain = 0, Sculpt = 1, SculptFromEntity = 2 };

    Kind              kind = Kind::Terrain;
    std::string       name;
    DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};

    // Terrain
    u32 resolution = 128;
    f32 worldSize  = 200.0f;
    f32 maxHeight  = 200.0f;

    // Sculpt（素体）
    int primitive    = 1;    // SculptPrimitive（0=Box,1=Sphere,2=Plane,3=Cylinder）
    u32 subdivisions = 16;
    f32 size         = 2.0f;

    // SculptFromEntity（既存モデルを編集可能にする）
    entt::entity source = entt::null;

    McpDeferred mcp;
};

struct PendingScriptAttach
{
    entt::entity entity = entt::null;
    std::string  scriptPath;   // assets 相対パス
};

// アセットブラウザからテクスチャをドラッグ&ドロップしてマテリアルに割り当てる要求
// (SceneView 上のメッシュへドロップ、または Inspector のマテリアルスロットへドロップ)。
// テクスチャロード(GetOrLoadTexture)が cmdList を要するためフレーム境界で処理する。
enum class MaterialTextureSlot { Albedo, Normal, MetalRoughness };
struct PendingMaterialTextureDrop
{
    entt::entity entity = entt::null;
    u32 submeshIndex = 0;              // MeshRenderer::meshes[] のインデックス
    MaterialTextureSlot slot = MaterialTextureSlot::Albedo;
    std::string texturePath;           // 絶対パス(処理側で assets 相対に正規化)
};

// Inspector の「モデル差し替え」要求。モデルロード(GetOrLoadModel)が cmdList を
// 要するためフレーム境界で処理する(SceneSerializer::SwapEntityModel)。
struct PendingModelSwap
{
    entt::entity entity = entt::null;
    std::string  newModelPath;         // 絶対 or assets 相対(処理側で正規化)
};


class EditorContext
{
public:
    // ---- マルチ選択 ----
    entt::entity selectedEntity = entt::null;   // プライマリ（後方互換）
    std::vector<entt::entity> selectedEntities; // 全選択リスト

    bool IsSelected(entt::entity e) const
    {
        return std::find(selectedEntities.begin(), selectedEntities.end(), e)
               != selectedEntities.end();
    }

    void Select(entt::entity e)
    {
        selectedEntities.clear();
        selectedEntity = e;
        if (e != entt::null)
            selectedEntities.push_back(e);
    }

    void AddToSelection(entt::entity e)
    {
        if (e == entt::null) return;
        if (!IsSelected(e))
            selectedEntities.push_back(e);
        selectedEntity = e;
    }

    void ToggleSelection(entt::entity e)
    {
        if (e == entt::null) return;
        auto it = std::find(selectedEntities.begin(), selectedEntities.end(), e);
        if (it != selectedEntities.end())
        {
            selectedEntities.erase(it);
            selectedEntity = selectedEntities.empty() ? entt::null : selectedEntities.back();
        }
        else
        {
            selectedEntities.push_back(e);
            selectedEntity = e;
        }
    }

    void ClearSelection()
    {
        selectedEntity = entt::null;
        selectedEntities.clear();
    }

    bool HasSelection() const { return !selectedEntities.empty(); }

    // ギズモ
    GizmoMode gizmoMode      = GizmoMode::Translate;
    bool      gizmoLocalSpace = false;

    // ギズモのスナップ量（従来はハードコードやった。エンジン設定ウィンドウで編集する）。
    // 既定値は元のハードコード値と同じ＝挙動は変わらない。
    f32  snapTranslate  = 1.0f;    // m
    f32  snapRotateDeg  = 15.0f;   // degree
    f32  snapScale      = 0.1f;    // 倍率
    bool snapAlways     = false;   // true なら Ctrl 押下なしでも常にスナップ

    // ビューポートツールのフック。SceneViewPanel が UI 編集/3D ピッキングより前に
    // 先頭から順に呼び、true を返した時点で打ち切って以降の編集操作を行わない。
    // （地形ブラシ、ライトのハンドル操作などがここに登録する。ViewportInput 参照）
    std::vector<std::function<bool(const ViewportInput&)>> viewportToolHandlers;

    // ---- Application から借りている読み取り専用データ（Application::Initialize が設定）----
    // 今フレームの描画リスト。精密ピッキングのブロードフェーズ候補に使う（null なら
    // SceneViewPanel が従来どおり entt 走査へフォールバックする）。
    const std::vector<DrawItem>* drawItems = nullptr;
    // Application::m_cpuMs の先頭。CpuPicking / CpuGizmo をここへ加算すると
    // MCP の dx12_perf_stats の cpuScopeMs に出る。null なら計測しない。
    f32* cpuScopeMs = nullptr;

    // 2D ビューモード（Unity の 2D ボタン相当）。ON 中はエディタカメラを正射＋XY平面正対に固定し、
    // 回転/ドリーを禁止（中ドラッグでパン、ホイールでズーム）。ギズモも正射モードで表示する。
    bool  view2D     = false;
    float view2DZoom = 6.0f;   // 正射の縦半分（世界単位）。小さいほどズームイン。

    // クラスタードライティング（Forward+）のデバッグ表示。
    // 0=off / 1=ライト複雑度ヒートマップ（青→緑→赤。上限 128 灯に張り付いた所は白）/
    // 2=クラスタ境界。Application::Render が毎フレーム読んで b1 の clusterExtra.z へ流す。
    // 「1 クラスタ 128 灯を超えて無言で切り捨てられている場所」を見つける唯一の手段なので、
    // 診断（DeepDiagnostics の lighting）と dx12_list_lights の警告からここへ誘導している。
    u32 clusterDebugMode = 0;

    // ---- DXR（レイトレーシング）の実行時状態。Application::Render が毎フレーム書き、
    //      ライティング窓の「レイトレーシング (DXR)」節が読むだけ（編集はしない）。
    //      ★非対応 GPU でチェックボックスを黙って無効にするのが最悪なので、
    //        BeginDisabled + 理由表示に必要な値をここへ流す（計画09 §5.3）。
    bool dxrSupported      = false;   // 6 段ゲートを全部通ったか
    int  dxrTier           = 0;       // D3D12_RAYTRACING_TIER の実値（0=非対応 / 10 / 11 / 12）
    int  dxrShaderModel    = 0;       // D3D_SHADER_MODEL の実値（0x65 = SM 6.5）
    u32  dxrInstances      = 0;       // 直近フレームで TLAS に入ったインスタンス数
    u32  dxrSkippedSkinned = 0;       // TLAS から外したスキンド数（CSM が担当する）
    u64  dxrAsBytes        = 0;       // 加速構造の VRAM 合計

    // UI 編集モード（ツールバーの「UI」トグル）。ON 中は非 Play でも SceneView に
    // ゲーム内 UI（UICanvas ツリー）をプレビュー描画し、UI 要素のクリック選択・
    // ドラッグ編集を 3D ピッキングより優先する。Play 中/ゲームモードは従来どおり
    // ##GameUI 経路のみが描く（二重描画しない）。
    bool uiEditMode = false;

    // メニュー「表示 > レイアウトをリセット」で立てるフラグ。
    // EditorLayer が検知してデフォルトドックレイアウトを再構築する。
    bool resetLayout = false;

    // 編集用の照らし込み（F2 でトグル）。シーンの環境光に「下限」として被せるだけで、
    // シーンには保存されず Play 中は無効＝ゲームの絵は暗いまま。暗い屋内シーンを
    // 触るときに「見えないから置けない」を潰すためだけのもの。
    // 0 で無効。0.35 くらいで真っ暗な部屋の形が読めるようになる。
    float viewportFill = 0.0f;

    // ---- ツール窓の表示トグル（既定OFF=スッキリ中核4窓。ツールバー「窓」/メニュー「ウィンドウ」から開閉）----
    // ON の窓だけ Inspector の下（右カラム下部）にタブで現れる。全OFFなら Inspector が右カラム全高。
    bool showPostProcess    = false;
    bool showPostParams     = false;
    bool showSkybox         = false;   // Skybox / IBL
    bool showSSAO           = false;
    bool showScreenSpaceGi  = false;   // SSR / SSGI 設定窓
    bool showVolumetricFog  = false;   // ボリュメトリックフォグ設定窓
    bool showEngineSettings = false;
    bool showSceneFlow      = false;
    bool showProject        = false;
    bool showVersionControl = false;
    bool showMcpBridge      = false;   // MCP / AI Bridge モニタ窓
    bool showBuildSettings  = false;   // ビルド設定（Unity の Build Settings 相当）
    bool showNetworkStatus  = false;   // マルチプレイ状態モニタ窓（接続一覧/tick/帯域、フェーズ⑧）
    bool showNetworkSettings = false;  // マルチプレイ設定窓（tickRate/port等、assets/network.json、フェーズ⑨）

    // ---- マルチプレイのローカルテストループ（ツールバーPlayドロップダウン、フェーズ⑨）----
    NetTestRole netTestRole = NetTestRole::Offline;   // 次にPlayした時に自動でHost/Joinするか
    std::string netTestJoinAddress = "127.0.0.1";     // Client選択時の接続先IP
    u16 netTestJoinPort = 0;                          // 0=NetworkConfig.defaultPortを使う。--net-client CLI 起動時のみ明示指定
    bool netTestLaunchClientRequested = false;        // 「テストクライアント起動」ボタン押下(フレーム境界でCreateProcess)
    // パーティクルエディタ（VFXアセットの作成・編集）。プレビュー枠+アセット一覧+パラメータで
    // 幅を食うため、右下タブ群（右カラム24%のさらに下42%）には収まらない。他の設定系ツール窓と
    // 違い右下タブにはドックせず独立したフローティング窓として開く＝ AnyToolWindowOpen には含めない
    // （含めると右下タブ領域だけ確保されて中身が無い＝Inspectorが無駄に縮む）。
    bool showVfxEditor      = false;
    // マテリアルエディタ/マテリアルライブラリ(Poly Havenダウンローダー)も VfxEditor 同様、
    // 独立フローティング窓として開く(AnyToolWindowOpen には含めない)。
    bool showMaterialEditor  = false;
    bool showMaterialLibrary = false;
    // UIエディタ(ゲーム内UIの2Dキャンバス編集。UMGデザイナー相当)。広い面積が要るので
    // VfxEditor 同様の独立フローティング窓(AnyToolWindowOpen には含めない)。
    bool showUiEditor        = false;
    // UIアニメーションのタイムラインエディタ(.uianim のキーフレーム編集)。横に長い窓が要るので
    // これも独立フローティング(AnyToolWindowOpen には含めない)。
    bool showAnimEditor      = false;
    // スプライトシートエディタ(.spranim の分割/フレーム選択/シーケンス編集)。同上。
    bool showSpriteSheetEditor = false;
    // AssetBrowser で .dxmat をダブルクリックした時に立つ。MaterialEditorPanel が消費して開く。
    std::string pendingOpenMaterialPath;
    // 同じく .uianim / .spranim のダブルクリック。各パネルが消費して開く。
    std::string pendingOpenUiAnimPath;
    std::string pendingOpenSpriteSheetPath;

    // プレハブのリンク操作。どちらもエンティティの生成/破棄を伴うので、
    // 他の pending* と同じくフレーム境界（Application）で実行する。
    std::vector<entt::entity> pendingPrefabApply;    // インスタンス → 元 .prefab へ書き戻す
    std::vector<entt::entity> pendingPrefabRevert;   // インスタンス ← 元 .prefab で作り直す
    // Apply 後に「他のインスタンスも更新する」を押した時だけ立てる（伝播元を除いて Revert）
    std::vector<entt::entity> pendingPrefabPropagate;

    bool AnyToolWindowOpen() const
    {
        return showPostProcess || showPostParams || showSkybox || showSSAO || showScreenSpaceGi
            || showVolumetricFog
            || showEngineSettings || showSceneFlow || showProject || showVersionControl
            || showMcpBridge || showBuildSettings || showNetworkStatus || showNetworkSettings;
    }

    // タッチパッド向けキーボードフライモード（` キーでトグル）。
    // ON 中は WASD 移動 / Q・E 上下 / 矢印キーで視点回転（マウス・ボタン長押し不要）。
    // ON 中はギズモの W/E/R 切替ショートカットを抑制する。
    bool flyMode = false;

    // 3D ビューポート（ドックスペース中央ノード）の矩形（ImGui 座標）。
    // EditorLayer が毎フレーム更新。Application のフライカメラ発動判定
    // （カーソルが 3D ビュー上にあるか）に使う。ImGui の PassthruCentralNode の
    // WantCaptureMouse 挙動に依存せず確実にビュー上判定するための値。
    f32 viewportX = 0.0f;
    f32 viewportY = 0.0f;
    f32 viewportW = 0.0f;
    f32 viewportH = 0.0f;

    // 描画統計（前フレーム。Application::BuildDrawList が書く。ビューポートHUD表示用）。
    // statDraws = 全シーンパス(影/プリパス/メイン/プレビュー)の DrawIndexedInstanced 発行数、
    // statCulled = 同パス群でフラスタムカリングにより省いたエンティティ描画の延べ数。
    u32 statDraws  = 0;
    u32 statCulled = 0;

    // シーンの段階ロードの進捗 0..1（Application が毎フレーム書く。-1 = ロード中でない）。
    // ステータスバーが読む＝重いシーンでも「止まった」ではなく「読んでいる」と分かる。
    f32 sceneLoadProgress = -1.0f;

    // フローティングツール窓(マテリアルエディタ/マテリアルライブラリ/パーティクルエディタ等、
    // ImGuiWindowFlags_NoDockingで浮かぶ独立窓)がホバーされているか。
    // これらの窓は 3D ビューポート矩形の上に重なって浮かぶことがあるため、IsCursorInViewport が
    // 矩形だけで判定すると「窓の上でドラッグ/ホイール操作した」のがそのままシーンカメラにも
    // 効いてしまう(窓に隠れているだけの3Dビューを触っている扱いになる)。ここで弾く。
    //
    // 2値ラッチにしている理由: 各パネルが ThisFrame を立てるのは ImGui パスの「後半」
    // (Application::Render の EditorLayer::Render より後)なので、同一フレーム内のリセット+読み取り
    // だと HandleCameraNavigation は常に false を読んでしまう。フレーム冒頭で前フレームの
    // ThisFrame を hovered へ確定コピーし、読む側は常に「前フレームの確定値」を見る(1フレーム遅延)。
    bool floatingToolWindowHovered = false;           // 読み取り用(前フレームの確定値)
    bool floatingToolWindowHoveredThisFrame = false;  // 書き込み用(各パネルが今フレーム立てる)

    bool IsCursorInViewport(f32 mx, f32 my) const
    {
        return !floatingToolWindowHovered
            && viewportW > 0.0f && viewportH > 0.0f
            && mx >= viewportX && mx < viewportX + viewportW
            && my >= viewportY && my < viewportY + viewportH;
    }

    // カメラプレビュー用テクスチャの GPU ハンドル（ImTextureID）。
    // Application が選択カメラ視点を描画してセット。0 ならプレビュー非表示。
    // EditorLayer がシーンビュー隅に ImGui::Image でオーバーレイ表示する。
    u64 cameraPreviewTexHandle = 0;

    // シーンパス
    std::string currentScenePath;

    // 通知フラッシュ
    f32 hotReloadFlash    = 0.0f;
    f32 buildCompleteFlash = 0.0f;
    f32 buildErrorFlash    = 0.0f;  // ビルド失敗表示（>0 の間 赤で「✗ ビルド失敗」）
    std::string buildErrorMsg;      // ビルド失敗の具体理由（空なら汎用メッセージ）

    // エラー通知（Play 不可等）
    std::string errorMessage;
    f32 errorFlash = 0.0f;

    // 遅延処理キュー
    std::vector<PendingSpawnRequest> pendingSpawns;
    std::vector<entt::entity>        pendingDuplications;  // Ctrl+D / 右クリック複製
    std::vector<std::string>         pendingPastes;        // Ctrl+V (エンティティJSON)
    std::vector<entt::entity>        pendingDeletions;
    std::vector<McpPendingDelete>    mcpDeletions;         // MCP 由来削除（応答に deletedCount を返す）
    std::vector<McpPendingDelete>    mcpDuplications;      // MCP 由来複製（.entity=複製元。応答に複製先 id を返す）
    // MCP 由来の地形/スカルプト生成（GPU メッシュ構築に cmdList が要るのでフレーム境界で処理）
    std::vector<McpPendingProcCreate> mcpProcCreates;
    // Undo/Redo はエンティティ復元（モデル再ロード）を伴う場合があるため
    // cmdList が有効なフレーム境界まで遅延する
    bool pendingUndo = false;
    bool pendingRedo = false;
    std::vector<PendingScriptAttach> pendingScriptAttachments;
    std::vector<PendingMaterialTextureDrop> pendingMaterialTextureDrops;
    // 選択エンティティ（+子孫）を .prefab として書き出す要求。Application がフレーム境界で処理。
    entt::entity pendingCreatePrefab = entt::null;
    // ヒエラルキーの「グループ化」(Ctrl+G)。選択をまとめる空の親を作る要求。
    // 空の親は原点・無回転・スケール1で作るので、子のワールド位置は一切動かない。
    bool pendingGroupSelection = false;
    // 生成直後のエンティティ名をその場で入力させたいときに Application が入れる。
    // HierarchyPanel が拾ってインライン編集を開始し、null に戻す。
    entt::entity requestRenameEntity = entt::null;
    std::string pendingLoadPath;
    std::string pendingGameLoadPath;  // Play 中の loadScene()（assets 相対）。フレーム境界で安全にロード
    bool pendingBuildGame = false;     // ビルド設定パネルの「ビルド」で立つ＝実行要求
    // ファイルメニュー「プロジェクトを閉じる」で立つ。Application がフレーム境界で
    // ランチャーへ戻す（削除等のファイル操作は一切不要＝現在の状態はそのままメモリに残す）。
    bool pendingCloseProject = false;
    // ★true なら未保存の確認モーダルを出さずに素通しする。立てるのは2種類:
    //   (a) MCP 由来 … AI はモーダルを押せないので、出すとツール呼び出しが固まる。
    //       MCP 側は dx12_ping の sceneDirty を見て自分で判断する契約（index.ts に明記）。
    //   (b) プロジェクト読み込み由来 … ロード中/ランチャー中は EditorLayer が呼ばれない＝
    //       モーダルを描く場所が無い。ここで止めると pendingLoadPath が消化されないまま残り、
    //       以後の open_scene が「already in progress」で永久に失敗する（実際に踏んだ）。
    //       プロジェクト切替の確認は BeginProjectLoad を呼ぶ「前」に UI 側で行うのが正しい。
    bool pendingLoadSkipConfirm  = false;
    bool pendingNewSceneSkipConfirm = false;
    // 直近ビルドの成果物フォルダ（完了後に Explorer で開くために保持）。
    std::string lastBuildDir;

    // ===== ビルド設定（Unity の Build Settings / Unreal の Packaging に相当）=====
    // ビルド設定ウィンドウ（showBuildSettings）で編集し、BuildGame() が参照する。
    struct BuildConfig
    {
        char title[128] = "Game";           // 製品名（game.pak マニフェストの title・ゲーム窓のタイトル）
        int  width  = 1280;                 // ゲームウィンドウ幅
        int  height = 720;                  // ゲームウィンドウ高
        std::string startScene;             // 開始シーン（assets 相対。空=現在開いているシーン）
        std::string outputDir;              // 配置先の親フォルダ（空=ビルド時に選択）
        bool openFolderAfterBuild = true;   // 完了後に成果物フォルダを開く
    } buildConfig;

    bool pendingNewScene  = false;
    // 空でなければ pendingNewScene 実行時にこのパスへスターターシーンを保存する
    // （プロジェクト新規作成の初期シーン用。通常の「新規シーン」は空のまま）
    std::string pendingNewScenePath;
    bool showNewSceneDialog = false;
    bool newSceneDialogIsCreate = true;  // true=新規作成, false=名前を付けて保存
    char newSceneNameBuf[128] = {};

    // スクリプト作成ダイアログ
    bool showNewScriptDialog = false;
    char newScriptNameBuf[128] = {};

    // カスタムシェーダー作成ダイアログ
    bool showNewShaderDialog = false;
    char newShaderNameBuf[128] = {};

    // Undo/Redo
    UndoSystem undoSystem;

    // ── 未保存フラグ ──
    // 変更の検知経路は2つある。どちらか一方でも食い違えば未保存。
    //  (1) undoSystem.EditSeq() … 変更のたびに ++ される単調増加カウンタ。
    //      Undo を積む操作と、MCP の書き込み系メソッドがここを進める。
    //  (2) settingsHash … シーン設定（ポスト/SSAO/空/フォグ等）の指紋。
    //      これらを触る窓は 20 箇所以上あって Undo を積まないので、
    //      Application が定期的に SceneSettingsFingerprint() で更新する。
    // Play 開始/終了ではリセットしない（Play 前に未保存だったなら Stop 後も未保存）。
    u64  sceneSavedSeq      = 0;
    u64  settingsHash       = 0;   // Application が毎回上書きする「現在値」
    u64  savedSettingsHash  = 0;   // 保存時点の値

    bool IsSceneDirty() const
    {
        return undoSystem.EditSeq() != sceneSavedSeq || settingsHash != savedSettingsHash;
    }
    // ── 未保存の確認モーダル ──
    // showUnsavedConfirm が true の間 ToolbarPanel がモーダルを描く。
    // ユーザーが選ぶと unsavedChoice に入り、Application::ConfirmDiscardScene が消化する。
    // ★MCP 経由（open_scene / new_scene / open_project）はここを通さない。
    //   AI はモーダルを押せないので出すと固まる。あちらは dx12_ping の sceneDirty で判断させる。
    enum class UnsavedChoice { None, Save, Discard, Cancel };
    bool          showUnsavedConfirm = false;
    UnsavedChoice unsavedChoice      = UnsavedChoice::None;

    // 保存に成功したときに呼ぶ。freshSettingsHash は「いま保存した内容」の指紋。
    // ★ポーリングで拾った古い値ではなく、保存の直前に取り直した値を渡すこと。
    //   古い値を渡すと、保存直前の設定変更が保存後に「新しい変更」として再検出される。
    void MarkSceneSaved(u64 freshSettingsHash)
    {
        sceneSavedSeq     = undoSystem.EditSeq();
        settingsHash      = freshSettingsHash;
        savedSettingsHash = freshSettingsHash;
    }

    // クリップボード（Ctrl+C/V 用。エンティティの JSON スナップショット）
    std::vector<std::string> clipboard;

    // エディタ UI アイコン（Application が所有・populate。null/0 ならテキスト表示にフォールバック）
    const EditorUiIcons* icons = nullptr;

    // Inspector のモデル差し替え（フレーム境界で SwapEntityModel が処理）。
    // 【2026-07-20 追記・解決済み】かつてここに「メンバを中間に挿入するとヒープ域外書き込みが
    // pendingLoadPath を直撃してクラッシュするので必ず末尾へ足すこと」という注意書きがあったが、
    // 真因は域外書き込みではなく ninja のヘッダー依存の再コンパイル取りこぼしだった。
    // 古い .obj が旧レイアウトのオフセットへ書き、新しい .obj が新オフセットから読むことで
    // 「floatパターンでメンバが壊れる」ように見えていた(v1.4.4 の EditorLayer.cpp.obj でも再発)。
    // → メンバはどこへ足してもよい。ただしヘッダーを変更した日は build/release を消して
    //    クリーンビルドすること(installer/build.ps1 が stale obj を検出して配布を止める)。
    std::vector<PendingModelSwap> pendingModelSwaps;

    // エンジン診断パネル（UI 自動テストの実行・結果表示。ツール > エンジン診断）
    bool showEngineDiagnostics = false;

    // 地形ツール窓（ハイトフィールド地形の作成 / スカルプトブラシ / 浸食 / 山の一発生成）。
    // VfxEditor 等と同じ独立フローティング窓なので AnyToolWindowOpen には含めない。
    // ヒエラルキー「＋エンティティ追加 → 地形」で開き、Terrain 付きエンティティを選ぶと自動で開く。
    bool showTerrainEditor = false;

    // ===== ライティング編集（editor/LightHandles.* / editor/panels/LightingPanel.*）=====
    // ライティング・パネル（シーンの光を1画面で詰める）。VfxEditor 等と同じ独立フローティング窓
    // として開くので AnyToolWindowOpen には含めない＝右下タブ領域を占有しない。
    bool showLighting = false;
    // シーンビューで全ライトの影響範囲（点=半径リング / スポット=コーン）を薄いワイヤで常時表示。
    // 選択中でなくても見えるので「どこまで光が届くか」を一望できる。
    bool lightWireAll = false;
    // ライティング・パネルの「環境マップを適用 / 再ベイク」で立つ。Application がフレーム境界で
    // 消費し、m_loadedSkyboxPath をクリアして IBL を焼き直す（Skybox / IBL 窓のボタンと同じ動作）。
    bool pendingSkyboxRebake = false;

    // スカルプト窓（任意メッシュの頂点スカルプト。洞窟・アーチ・岩などの異形）。
    // 地形ツール窓と同じ独立フローティング窓なので AnyToolWindowOpen には含めない。
    // ヒエラルキー「＋エンティティ追加 → スカルプト」で開き、SculptMesh 付きを選ぶと自動で開く。
    bool showSculptEditor = false;
};

} // namespace dx12e
