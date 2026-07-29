#pragma once

#include <string>
#include <vector>

// nlohmann::json は前方宣言だけを引く（このヘッダを読むだけの側に json.hpp の
// コンパイル時間を押し付けないため）。RunAll の戻り値を実際に使う .cpp では
// <nlohmann/json.hpp> を自分で include すること。
#include <nlohmann/json_fwd.hpp>

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

// シーンの光。灯数の上限超過（クラスタード: 点+スポット合計 1024 灯 / 1 クラスタ 128 灯。
// 超えたぶんは *無言で* 描画されない）、
// DirectionalLight が 0 個 / 2 個以上（2 個目以降は無視される）、影スロット超過、
// 強度 0・真っ黒・range 0 の「効いていないライト」、コーン角の内外逆転、
// IBL 未設定のまま金属マテリアルを置いている、を検出する。
DeepDiagReport Lighting(Application& app);

// ハイトフィールド地形。.hf の存在／ヘッダ整合／サイズ、コンポーネントとの解像度食い違い、
// Jolt の要求（サンプル数が 4 の倍数）、コライダー用 RigidBody の有無、
// 複数の地形が同じ .hf を共有していないか（片方を彫るともう片方が壊れる既知の罠）。
DeepDiagReport Terrain(Application& app);

// 三角形精密ピッキングが破綻する条件。CPU インデックス／位置キャッシュを持たないメッシュ
// （AABB 判定に落ちる）、スケール 0・NaN の Transform（逆行列が作れずレイが素通り）、
// AABB が NaN/巨大、原点からジオメトリが極端にズレたモデル（カリング/LOD の誤動作）。
DeepDiagReport Picking(Application& app);

// 自動 GPU インスタンシング。settings.json の "render_instancing"、適格ドローの割合、
// そして「不適格になっている支配的な理由」のランキング（性能を詰めるときに一番効く情報）。
// ※ 今フレームの描画リスト（Application::GetDrawItems）を読む＝1 度でも描画された後に呼ぶこと。
DeepDiagReport Instancing(Application& app);

// assets 配下と scripts 配下の .lua を字句だけ追って検査する。
// 文字列/長括弧/コメントを正しく飛ばした上で end・until・括弧の対応を数えるので、
// 「閉じ忘れ」「文字列が閉じていない」という一番多い壊れ方を実行せずに拾える。
// 式の文法までは見ない（誤検知で狼少年にしないため）。
DeepDiagReport Scripts();

// DXR（レイトレーシング）。GPU の Tier / シェーダモデル、6 段ゲートの通過状況、
// TLAS のインスタンス数と加速構造の VRAM 使用量、そして「TLAS に入らなかったもの」
// （スキンド / 半透明 / 上限超過）を出す。
// 「RT 影を ON にしたのにキャラの影が変わらない」「なぜか一部の物だけ影が出ない」を
// 説明できる唯一の場所（スキンドは仕様として CSM が担当する）。
DeepDiagReport Dxr(Application& app);

// 「シーンビューが真っ暗」「カメラが何も映さない」の原因を名指しする検査。
// 絵が出ない理由はどれも単独では静かに壊れる（ログにも出ない）ので、まとめてここで見る:
//  - デバッグ可視化(render_debug)が出しっぱなし … RtDiff 等は仕様として真っ黒
//  - 露出 0 / ティントが黒 / 自動露出の範囲が逆転 … ポストの掛け算で最終色が 0 になる
//  - 空も IBL も太陽も全部 0 … 光源が 1 つも無い
//  - シーン矩形が潰れている … 画面にはクリア色だけが残る
//  - SRV ヒープ枯渇寸前 … 次のロードで例外 → 描画が止まって戻らなくなる
//  - MCP がゲームカメラを握ったまま / カメラが NaN・極端な座標
DeepDiagReport RenderHealth(Application& app);

// ===================== 機械可読な一括実行（MCP: dx12_diagnose 用）=====================
//
// 全検査をまとめて回して JSON を返す。JUnit XML / 終了コード / 診断パネルの表示とは
// 独立した経路なので、既存の出力は一切変わらない。
//
// シグネチャ:
//     nlohmann::json DeepDiag::RunAll(Application& app, const std::string& only = "");
//
//   only … 空なら全検査。カンマ区切りで検査 ID を並べると、その ID だけを回す
//           （例 "lighting,terrain"）。未知の ID は無視され summary.unknownIds に載る。
//           検査 ID の一覧は AllCheckIds()。重い検査（textures / models）を外したい
//           MCP 呼び出しのためにある。
//
// 戻り値（キーは全て必ず存在する。checks の並びは AllCheckIds() の順）:
// {
//   "version": 1,                       // このスキーマの版。形を変えたら上げる
//   "engine":  "1.4.5",                 // dx12e::kEngineVersion
//   "checks": [
//     {
//       "id":       "lighting",         // AllCheckIds() の要素
//       "title":    "ライティング",     // 日本語の表示名
//       "checked":  12,                 // 実際に検査した対象の数
//       "errors":   1,                  // level==2 の件数（表示を打ち切っても正確）
//       "warnings": 3,                  // level==1 の件数
//       "infos":    2,                  // level==0 の件数
//       "issues": [ { "level": 2, "text": "…日本語 1 行…" } ],   // 先頭 40 件まで
//       "omitted":  0,                  // 上限を超えて issues から落とした件数
//       "skipped":  ""                  // 全件見ていない場合の理由（空＝全件見た）
//     }
//   ],
//   "summary": {
//     "checks":     10,                 // 回した検査の数
//     "errors":     1,                  // 全検査の合計
//     "warnings":   7,
//     "infos":      9,
//     "ok":         false,              // errors == 0
//     "unknownIds": []                  // only に未知の ID があれば入る
//   }
// }
//
// 呼び出し側への注意（MCP から使うとき）:
//  ・メインスレッドから呼ぶこと（entt / Scene を直接読む。GPU には触らない）。
//  ・"instancing" は Application::GetDrawItems() を読む＝最低 1 フレーム描画した後に呼ぶこと。
//    まだ空なら適格率は測らず skipped に理由が入る（エラーにはならない）。
//  ・"textures" / "models" は assets 配下を全部 Probe するのでアセット数によっては数十秒かかる。
//    応答時間が要るなら only で外すこと（例 only="lighting,terrain,picking,instancing,scripts"）。
//  ・失敗判定は summary.errors > 0（= summary.ok == false）だけを見ればよい。
//    注意(level 1)/情報(level 0)は失敗ではない（誤検知で赤くしない方針）。
nlohmann::json RunAll(Application& app, const std::string& only = "");

// RunAll が回す検査 ID を実行順で返す。
// "shaders","textures","models","gamma","scene_assets","lighting","terrain",
// "picking","instancing","scripts"
std::vector<std::string> AllCheckIds();

} // namespace DeepDiag
} // namespace dx12e
