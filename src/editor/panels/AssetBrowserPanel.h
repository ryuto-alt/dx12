#pragma once

#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>
#include "core/Types.h"
#include "editor/AssetBrowserLogic.h"
#include "editor/AssetIndex.h"
#include "editor/ThumbnailCache.h"

struct ID3D12GraphicsCommandList;
struct ImDrawList;

namespace dx12e
{

class EditorContext;
class ResourceManager;
class DescriptorHeap;
class ModelThumbnailRenderer;
class MaterialPreviewRenderer;

// ===== アセットブラウザ（下部ドックのタブ）=====
// 一覧の走査は AssetIndex（ワーカースレッド + ReadDirectoryChangesW）、サムネイルは ThumbnailCache（非同期デコード +
// ディスクキャッシュ）/ ModelThumbnailRenderer / MaterialPreviewRenderer。UI スレッドは「可視セルの描画」だけをする
// （ImGuiListClipper 相当の行クリッピング）。純ロジックは editor/AssetBrowserLogic.h。
class AssetBrowserPanel
{
public:
    AssetBrowserPanel();
    ~AssetBrowserPanel();

    void Initialize(const std::string& assetsDir,
                    const std::string& scriptsDir,
                    ResourceManager* resourceManager,
                    DescriptorHeap* srvHeap);

    void Render(EditorContext& ctx, f32 dt);
    void ForceRefresh();

    // プロジェクト切替時にルートを差し替える
    void SetRoots(const std::string& assetsDir, const std::string& scriptsDir);
    void SetThumbnailRenderer(ModelThumbnailRenderer* r);
    // .dxmat の球体サムネイル用(MaterialEditorPanel が所有する MaterialPreviewRenderer を借りる)
    void SetMaterialPreviewRenderer(MaterialPreviewRenderer* r) { m_materialPreview = r; }

    // クイックオープン(Ctrl+P)用: 指定のアセットがあるフォルダへ移り、そのアセットを選択状態にする。
    // 検索欄は空に戻す（検索結果に隠れて見つからない、を防ぐ）。
    void RevealAsset(const std::filesystem::path& absPath);
    // アセット/スクリプトのルート（クイックオープンの索引用）
    const std::filesystem::path& AssetsRoot()  const { return m_assetsRoot; }
    const std::filesystem::path& ScriptsRoot() const { return m_scriptsRoot; }
    // フレーム先頭でcmdListが有効な間に呼ぶ（サムネイルのアップロード / モデルサムネイルの描画用）
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    // ドラッグ&ドロップ用ペイロード型名
    static constexpr const char* kDragDropPayloadType = "ASSET_PATH";
    // アセットの移動専用ペイロード（フォルダ・シーン等、シーン/インスペクタへは落とせない物と、複数選択用）。
    // データは UTF-8 でなく ACP のパスを '\n' で連結した C 文字列。
    static constexpr const char* kMovePayloadType = "ASSET_MOVE";

    // テクスチャD&D(マテリアル割当)等、他パネルからも拡張子種別を判定したいので公開。
    // 並びは abl::Kind と同じ（CommandPalette が int へキャストして使う）。
    using AssetType = abl::Kind;
    static AssetType ClassifyExtension(const std::string& ext) { return abl::ClassifyExtension(ext); }

    // 他パネル(InspectorPanelのマテリアルテクスチャプレビュー等)から、このパネルが持つ
    // サムネイルキャッシュを使い回すための公開アクセサ。キャッシュ済みならGPUハンドルを即返す。
    // 無ければ非同期デコードのキューに積んで 0 を返す(数フレーム後に用意される)。サムネイルは長辺 128px。
    u64 GetOrQueueThumbnail(const std::string& absPath);

    // ---- 計測 / UI 自動テスト用 ----
    // 直近フレームの Render() の CPU 時間 [ms]（ImGui への積み込みまで。フレーム全体ではない）。
    f32 LastRenderMs() const { return m_lastRenderMs; }
    // 現在のフォルダを移す（検索は解除）。
    void NavigateTo(const std::filesystem::path& dir);
    const std::filesystem::path& CurrentDir() const { return m_currentDir; }
    size_t EntryCount() const { return m_entries.size(); }
    size_t VisibleCount() const { return m_view.size(); }
    size_t DrawnCellsLastFrame() const { return m_drawnCells; }
    bool   IsScanning() const { return m_index.IsScanning() || m_scanPending; }
    uint64_t ScanCount() const { return m_index.ScanCount(); }
    const ThumbnailCache& Thumbs() const { return m_thumbs; }

    // UI 自動テスト用の操作（ダイアログを介さず同じ経路を通る）
    bool TestCreateFolder(std::filesystem::path* outCreated = nullptr);
    bool TestRename(const std::filesystem::path& src, const std::string& newName);
    bool TestDuplicate(const std::filesystem::path& src, std::filesystem::path* outCopy = nullptr);
    bool TestMove(const std::filesystem::path& src, const std::filesystem::path& destDir);
    bool TestDelete(const std::filesystem::path& path);   // ごみ箱を使わず完全削除（使い捨てプロジェクト専用）
    void SetKindMask(uint32_t mask) { m_kindMask = mask; m_viewDirty = true; }
    uint32_t KindMask() const { return m_kindMask; }
    void SetListView(bool list) { m_listView = list; }
    bool ListView() const { return m_listView; }
    void SetSearch(const std::string& q);
    size_t SelectionCount() const { return m_sel.set.size(); }
    bool IsSelected(const std::filesystem::path& p) const { return m_sel.Has(p.string()); }
    void SelectOnly(const std::filesystem::path& p) { m_sel.Clear(); m_sel.set.insert(p.string()); m_sel.primary = p.string(); m_sel.anchor = p.string(); }

private:
    using Entry = abl::Entry;

    // ---- 走査 / 表示リスト ----
    void RequestScan();                       // 現在のフォルダ / 検索語で走査を要求
    void PollScan();                          // 結果が来ていれば取り込む
    void RebuildView();                       // フィルタ・ソート後の表示順を作る
    const Entry& EntryAt(int viewIndex) const; // viewIndex は m_view の添字
    bool HasUpEntry() const;

    // ---- 描画 ----
    void DrawToolbar(EditorContext& ctx);
    void DrawTree(bool& navigated);
    void DrawTreeNode(const std::filesystem::path& dir, const std::string& label, int depth, bool& navigated);
    void DrawContent(EditorContext& ctx);
    void DrawBreadcrumb();
    void DrawListHeader();
    void DrawGrid(EditorContext& ctx);
    void DrawGridCell(EditorContext& ctx, int viewIdx, float x, float y, float ts, float cw, float ch);
    void DrawListRows(EditorContext& ctx);
    void DrawDetailStrip();
    void DrawEmptyState();
    void DrawModals(EditorContext& ctx);
    void DrawBackgroundMenu(EditorContext& ctx);
    void DrawItemMenu(EditorContext& ctx, const Entry& e);
    void DrawRenameField(EditorContext& ctx);

    // セル / 行の共通処理（クリック・ダブルクリック・ドラッグ・コンテキスト・ドロップ）。直前の InvisibleButton に対して呼ぶ。
    void HandleItemInteraction(EditorContext& ctx, const Entry& e, int viewIndex, bool isUp);
    void OpenEntry(EditorContext& ctx, const Entry& e);
    void SetDragPayload(const Entry& e);
    void AcceptDropOnFolder(EditorContext& ctx, const std::filesystem::path& destDir);
    void DrawThumbOrGlyph(ImDrawList* dl, const Entry& e, float x, float y, float size, bool hovered);
    u64  ThumbHandleFor(const Entry& e);
    static DirectX::XMFLOAT3 SpawnPositionFrontOfCamera(const EditorContext& ctx);
    static void RevealInExplorer(const std::filesystem::path& p);

    void HandleKeyboard(EditorContext& ctx);
    void ProcessOsDrops(EditorContext& ctx);
    void PollImport(EditorContext& ctx);
    void StartImport(EditorContext& ctx, std::vector<std::filesystem::path> paths, bool placeInScene);
    void OpenImportDialog(EditorContext& ctx);

    // ---- ファイル操作 ----
    struct RelocateOp { std::filesystem::path src, dst; };
    struct PendingRelocate
    {
        std::vector<RelocateOp> ops;
        std::vector<std::string> refs;   // 参照しているファイル（相対パス）
        bool refsTruncated = false;
        bool isRename = false;
        bool open = false;               // 次のフレームでモーダルを開く合図
    };
    bool BeginRelocate(EditorContext& ctx, std::vector<RelocateOp> ops, bool isRename, bool checkRefs);
    bool ExecuteRelocate(EditorContext* ctx, const std::vector<RelocateOp>& ops);
    bool CreateFolderHere(std::filesystem::path* out, bool rename);
    bool CreateMaterialHere(std::filesystem::path* out);
    bool DuplicateEntry(EditorContext* ctx, const std::filesystem::path& src, std::filesystem::path* out);
    void StartRename(const std::filesystem::path& p, bool isNew);
    void CommitRename(EditorContext& ctx);
    void RequestDelete(std::vector<std::filesystem::path> paths);
    std::string AssetRelPath(const std::filesystem::path& p) const;   // assets 相対（scripts は "scripts/" 付き）。外なら空
    void OnRootsChanged();
    // UI からのフォルダ移動は描画中に一覧を消さないよう 1 フレーム遅らせる（セル描画ループの途中で m_view が空になるのを避ける）。
    void RequestNavigate(const std::filesystem::path& dir) { m_navTarget = dir; m_hasNav = true; }
    void SavePrefs();
    void LoadPrefs();
    // 現在の選択（表示順）
    std::vector<std::filesystem::path> SelectedPaths() const;

    // ---- マテリアルのプレビュー元テクスチャ（albedo）: 更新時刻つきキャッシュ（UI スレッドで毎回パースしない）----
    struct MatInfo
    {
        int64_t mtime = -1;
        std::string albedoAbs;
        int64_t albedoMtime = 0; uint64_t albedoSize = 0;
    };
    const MatInfo* ResolveMaterial(const Entry& e);
    std::unordered_map<std::string, MatInfo> m_matCache;
    int m_matResolvesThisFrame = 0;

    // ---- 状態 ----
    std::filesystem::path m_assetsRoot, m_scriptsRoot, m_currentDir;
    AssetIndex m_index;
    std::vector<Entry> m_entries;               // 直近の走査結果
    std::vector<int>   m_view;                  // 表示順の m_entries 添字（先頭が -1 なら「..」）
    std::vector<std::string> m_order;           // 表示順のパス（".." を除く）。選択の範囲計算用
    Entry m_upEntry;
    bool m_viewDirty = true;
    bool m_scanPending = false;
    bool m_searchTruncated = false;
    std::filesystem::path m_scannedDir;         // 走査結果が対応するフォルダ
    std::string m_scannedQuery;
    uint64_t m_lastTreeSerial = 0;

    char     m_searchBuf[64] = {};
    std::string m_appliedQuery;                 // 走査へ渡した検索語（入力のデバウンス用）
    f32      m_searchDebounce = 0.0f;
    uint32_t m_kindMask = 0;                    // 種別フィルタ（0 = すべて）
    abl::SortKey m_sortKey = abl::SortKey::Name;
    bool     m_sortAsc = true;
    bool     m_listView = false;
    f32      m_cellSize = 96.0f;                // 論理 px
    f32      m_treeWidth = 170.0f;              // 論理 px

    // 選択
    abl::Selection m_sel;
    int  m_cursor = -1;                         // 矢印キー移動の現在位置（m_order の添字）
    std::string m_clickPending;                 // 複数選択の 1 つを押した状態（ドラッグしなければ単独選択へ確定）
    std::string m_scrollToPath;                 // 走査後にこの項目が見える位置へスクロール（見つかるまで最大 90 フレーム待つ）
    int  m_scrollTries = 0;
    f32  m_setScrollY = -1.0f;
    bool m_cursorMoved = false;                 // 矢印キーで動いた → 描画側が見える位置へスクロール
    int  m_lastColumns = 1;                     // 直近のグリッドの列数（上下キー移動用）

    // 名前の変更（インライン）
    std::filesystem::path m_renamePath;
    char m_renameBuf[256] = {};
    int  m_renameWarmup = 0;
    bool m_renameIsNew = false;

    // 削除 / 移動の確認
    std::vector<std::filesystem::path> m_pendingDelete;
    bool m_deletePopupOpen = false;
    std::string m_deleteError;
    PendingRelocate m_relocate;
    bool m_relocateOpen = false;
    EditorContext* m_dropCtx = nullptr;         // Render 中だけ有効（ツリー / パンくずのドロップ先が使う）

    // 取り込み（別スレッドでコピー）
    struct ImportResult
    {
        int ok = 0, failed = 0, skipped = 0;
        std::vector<std::filesystem::path> created;
        std::string firstError;
        bool placeInScene = false;
    };
    std::future<ImportResult> m_import;
    bool m_importRunning = false;

    // 直近フレームの計測
    f32    m_lastRenderMs = 0.0f;
    size_t m_drawnCells = 0;

    // ツリー
    std::unordered_map<std::string, bool> m_treeOpen;
    std::string m_focusTreeLabel;

    // サムネイル
    ResourceManager* m_resourceManager = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;
    ThumbnailCache   m_thumbs;
    ModelThumbnailRenderer*  m_thumbRenderer = nullptr;
    MaterialPreviewRenderer* m_materialPreview = nullptr;
    bool m_prefsLoaded = false;
    std::filesystem::path m_navTarget;
    bool m_hasNav = false;
};

} // namespace dx12e
