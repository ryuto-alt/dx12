#pragma once

#include <string>
#include <vector>

// 超詳細診断（エンジン機能そのものの健全性チェック）。
//
// 既存の「エンジン診断」は ImGuiTestEngine で*エディタ UI を自動操作*して
// クラッシュ・フリーズを炙り出すもので、UI が動けば緑になる。
// つまり「シェーダーが古い」「テクスチャが壊れている」「法線マップが sRGB で保存されている」
// のような “動くけど絵が間違っている” 系はひとつも捕まえられない。
//
// ここはその穴を埋める側。GPU も ImGui も要らない純データ検査なので、
// UiTestHarness の検査項目からも将来のヘッドレス CLI からも同じものを呼べる。
//
// 方針:
//  - 1 件の指摘 = 1 行の日本語。開発者以外が読んでも次の一手が分かる文にする
//  - エラー(2)だけが検査失敗。注意(1)/情報(0) は出すだけで赤くしない（誤検知で狼少年にしない）
//  - 打ち切ったときは必ず skipped に理由と件数を書く（黙って一部だけ見て緑、を作らない）

namespace dx12e
{

class Application;

struct DeepDiagIssue
{
    int         level = 0;   // 0=情報 1=注意 2=エラー
    std::string text;        // 日本語 1 行
};

struct DeepDiagReport
{
    std::string                title;
    int                        checked = 0;   // 実際に検査した対象の数
    std::vector<DeepDiagIssue> issues;
    int                        omitted = 0;   // 表示上限を超えて捨てた指摘の数
    std::string                skipped;       // 全件見ていない場合の説明（空＝全件見た）
    int                        levelCounts[3] = {0, 0, 0};   // 表示を打ち切っても件数だけは正確に持つ

    // 上限（kMaxIssues）を超えたぶんは omitted に積むだけで捨てる。件数カウントは常に正確。
    void Add(int level, std::string text);

    int Count(int level) const;
    int Errors() const   { return Count(2); }
    int Warnings() const { return Count(1); }

    std::string Summary() const;   // "154 件検査 / エラー 2 / 注意 3"
};

namespace DeepDiag
{

// shaders/ の全 .cso が「存在し・壊れておらず・.hlsl より新しい」かを検査する。
// ビルドし忘れた古いシェーダーで動かしている、を検出するのが主目的。
DeepDiagReport Shaders();

// assets 配下の画像を全部 Probe して、読めない/サイズ 0/ミップ無しを洗う。
// 法線・metalRoughness が sRGB フォーマットで保存されていたらエラー（ライティングが確実に狂う）。
DeepDiagReport Textures();

// assets 配下のモデルを Assimp で Probe。メッシュ 0 / 頂点 0 / AABB が NaN /
// ボーン 256 超（SkinningBuffer が無言で切り捨てる）を検出する。
DeepDiagReport Models();

// 表示パイプラインのフォーマット構成が「リニア HDR → 手動ガンマ」の前提と合っているか。
// バックバッファが _SRGB になっていたらガンマ二重適用（＝全体が白っぽい）。
DeepDiagReport Gamma(Application& app);

// 現在のシーンが参照しているアセットが実際にロードできて描画対象になっているか。
// 「パスは合っているのに meshes が空＝画面に出ない」を捕まえる。
DeepDiagReport SceneAssets(Application& app);

} // namespace DeepDiag
} // namespace dx12e
