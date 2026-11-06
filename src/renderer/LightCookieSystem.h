#pragma once
// LightCookieSystem - スポットライトのクッキー（Light Function 相当）と IES プロファイルの GPU 側の置き場。
//
// 構造（バインドレスにしない理由: メイン RS の bindless は GPU 依存の任意機能で、フォワード PS が必須にできない）:
//   - クッキー: 256x256 RGBA8_SRGB の Texture2DArray（kMaxCookies スライス）。パス → スライスの対応を持つ。
//   - IES    : R16_FLOAT の Texture2D（幅 kIesSamples = 縦角 0..180°、高さ kMaxIes = IES ファイルの行）。最大 1.0 に正規化済み。
//   どちらも ClusteredLightCulling の slot11 テーブルの t31 / t32 に載る（ルート定数の DWORD は増えない）。
//   複数のスポットがそれぞれ別のクッキーを持てる（灯の ClusterLight.cookieIndex / iesIndex がスライス / 行を指す）。
//
// 使い方（Application）:
//   1. 毎フレーム Request(cookiePath, iesPath) を全スポットぶん呼ぶ（BeginRequests() の後）。新顔の画像だけ読む。
//   2. NeedsRebuild() なら GPU が空いている区間（WaitIdle 済み）で Rebuild(cmd) → 3 フレーム分のテーブルへ WriteSrvs。
//   3. CookieSlot / IesSlot で灯へ index を渡す。
#include <directx/d3d12.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class Texture;

class LightCookieSystem
{
public:
    static constexpr u32 kCookieSize = 256;
    static constexpr u32 kMaxCookies = 8;
    static constexpr u32 kIesSamples = 256;
    static constexpr u32 kMaxIes     = 8;

    LightCookieSystem();
    ~LightCookieSystem();

    // 開発時の「エンジン同梱アセット」置き場（dx12/assets/）。プロジェクトに無いパスのフォールバック。空なら使わない。
    void SetEngineAssetsDir(const std::string& dir) { m_engineAssetsDir = dir; }

    // フレームの要求集計を始める / 1 灯ぶんの要求を足す。空文字は無視。
    void BeginRequests();
    void Request(const std::string& cookiePath, const std::string& iesPath);

    // 新しい画像 / IES が増えた、または初回（ダミーを作る）。
    bool NeedsRebuild() const { return m_dirty; }

    // GPU テクスチャを作り直す。呼び出し側は WaitIdle 済みで、cmd は実行される前のコマンドリスト。
    void Rebuild(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd);

    // t31 / t32 のディスクリプタを書く（Rebuild 後に 3 フレームぶんのテーブルへ）。
    void WriteSrvs(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cookieDst, D3D12_CPU_DESCRIPTOR_HANDLE iesDst);

    bool IsReady() const { return m_cookieTex != nullptr && m_iesTex != nullptr; }

    // 灯へ渡す index。無い / 読めなかった / 満杯は -1。
    int CookieSlot(const std::string& path) const;
    int IesSlot(const std::string& path) const;

private:
    struct CookieEntry { std::string path; bool wanted = false; bool failed = false; };
    struct IesEntry    { std::string path; bool wanted = false; bool failed = false; };

    std::vector<u8> LoadBytes(const std::string& relPath) const;
    bool LoadCookiePixels(const std::string& path, u8* dst);
    bool LoadIesRow(const std::string& path, u16* dst);

    std::string m_engineAssetsDir;
    std::vector<CookieEntry> m_cookies;     // 添字 = スライス
    std::vector<IesEntry>    m_ies;         // 添字 = 行
    std::vector<u8>  m_cookiePixels;        // kMaxCookies * size * size * 4（未使用スライスは白）
    std::vector<u16> m_iesHalf;             // kMaxIes * kIesSamples（未使用行は 1.0）
    bool m_dirty = true;

    std::unique_ptr<Texture> m_cookieTex;
    std::unique_ptr<Texture> m_iesTex;
};

} // namespace dx12e
