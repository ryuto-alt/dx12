#pragma once

// ===========================================================================
// 更新の案内・進捗の窓（起動画面と同じ Direct2D + DirectWrite。per-pixel alpha の角丸カード + 柔らかい影）
// ---------------------------------------------------------------------------
//   案内   「新しいバージョンがあります  v2.0.0 → v2.1.0」+ GitHub リリース本文の先頭 6 行 + 「ほか N 件」
//          ボタン「今すぐ更新」「後で」「この版を飛ばす」
//   進捗   同じ窓がそのまま段階表示へ変わる: ダウンロード中（12.3 / 45.6 MB ・ 3.2 MB/s ・ 残り約 10 秒）→ 展開中 → 適用して再起動
//   失敗   窓の中で分かる言葉 + 「もう一度」「このまま起動」（MessageBox を出さない）
// 描画（Renderer）は窓も時計も持たない: 実窓（UpdateWindow）とプレビュー（--preview-update-ui）が同じ描画コードを通る。
// 古い exe がこの窓を動かすのは「この版を含む exe が出した次の更新」から（今の更新は古い exe の MessageBox で案内される）。
// 配色は ThemeVariants.h の Default（ネオン・エッジ）の値。窓は ImGui が立つ前に出るのでトークンを直接は引けない。
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/UpdateLogic.h"

namespace dx12e::updateui
{

// 論理寸法（100% 表示の px）
constexpr int kCardW = 680;
constexpr int kCardH = 440;
constexpr int kMargin = 48;                       // 影の逃げ
constexpr int kWindowW = kCardW + kMargin * 2;
constexpr int kWindowH = kCardH + kMargin * 2;

enum class Mode { Prompt, Downloading, Extracting, Applying, Error };

enum class Choice { None, UpdateNow, Later, SkipVersion, Retry, StartAnyway };

struct BodyLineW
{
    updatelogic::BodyLine::Type type = updatelogic::BodyLine::Type::Text;
    std::wstring text;
};

struct Model
{
    Mode mode = Mode::Prompt;
    std::wstring curVer, newVer;                  // "v2.0.0" / "v2.1.0"
    // 案内
    bool retrying = false;                        // 前回この更新を適用したのに反映されていない
    std::vector<BodyLineW> body;
    int more = 0;                                 // 「ほか N 件」
    bool bodyMissing = false;                     // 本文を取得できなかった
    // 進捗
    double fraction = -1.0;                       // 0..1。< 0 は不確定（回る弧）
    std::wstring detail;                          // 「12.3 / 45.6 MB ・ 3.2 MB/s ・ 残り約 10 秒」
    // 失敗
    std::wstring errTitle, errDetail;
    // 時計（秒。弧の回転・脈動）
    double time = 0.0;
};

struct Button
{
    Choice       id = Choice::None;
    std::wstring label;
    enum class Style { Primary, Secondary, Link } style = Style::Secondary;
    float x = 0, y = 0, w = 0, h = 0;             // 窓の論理座標（影の余白を含む）
};

// モードごとのボタンの並び。先頭ではなく Primary が「既定（Enter）」。
std::vector<Button> LayoutButtons(const Model& m);

// 案内窓用に GitHub 本文を整える（UTF-8 → Model の body / more / bodyMissing）。
void SetBodyFromMarkdown(Model& m, const std::string& markdownUtf8);

class Renderer
{
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // dpiScale: 表示倍率（1.0 = 100%）。logoPath: PNG（無くても動く）。失敗なら false（理由は LastError）。
    bool Init(float dpiScale, const std::wstring& logoPath);
    int  Width() const;                           // 窓全体の物理 px
    int  Height() const;
    // hover / pressed は LayoutButtons の添字（-1 = なし）。
    bool Draw(const Model& m, int hover, int pressed);
    bool CopyPixels(uint8_t* dst, int dstPitch);  // BGRA(premultiplied)
    const std::string& LastError() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------------------
// 実窓。メインスレッドで使う（更新確認はエンジン初期化より前の同期処理）。
// ---------------------------------------------------------------------------
class UpdateWindow
{
public:
    UpdateWindow();
    ~UpdateWindow();
    UpdateWindow(const UpdateWindow&) = delete;
    UpdateWindow& operator=(const UpdateWindow&) = delete;

    // 窓を作って表示（失敗なら false。呼び出し側は旧来の簡素な経路へ縮退する）。
    bool Create(const std::wstring& logoPath);

    // 案内を出し、ボタンが押されるまで待つ（戻り値: UpdateNow / Later / SkipVersion）。
    Choice Prompt(const std::string& curVer, const std::string& newVer, const std::string& bodyMarkdown, bool bodyFetched, bool retrying);

    // 進捗。どれもメッセージを処理して必要なら描き直す（呼び出し側が Pump を別に呼ぶ必要はない）。
    void SetDownloading(uint64_t done, uint64_t total, double bytesPerSec, double etaSec);
    void SetExtracting();
    void SetApplying();
    void Pump();                                 // 長い待ちの間に定期的に呼ぶ（展開の待受など）

    // 失敗を窓の中に出し、「もう一度」「このまま起動」を待つ（戻り値: Retry / StartAnyway）。
    Choice ShowError(const std::string& title, const std::string& detail);

    void Destroy();

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------------------
// 検証入口: DX12Engine.exe --preview-update-ui <prompt|downloading|extracting|applying|error|all> --out <png または all のとき出力ディレクトリ>
//                          [--dpi-scale N] [--update-body <md ファイル>] [--update-cur 2.0.0] [--update-new 2.1.0]
// 窓を作らず（人の画面に何も出さず）、実窓と同じ描画コードで PNG を書いて終了する。D3D12 / Application は初期化しない。
// argv に --preview-update-ui があれば実行して true を返し、exitCode に終了コード（0 = 成功）を入れる。無ければ false。
bool RunUpdateUiPreviewIfRequested(int argc, wchar_t** argv, int& exitCode);

} // namespace dx12e::updateui

namespace dx12e
{
using updateui::RunUpdateUiPreviewIfRequested;
}
