#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは Version.cpp の kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
//
// ★ 定数の実体は Version.cpp のみに置く（宣言だけをヘッダに置く）。
//   以前は constexpr でヘッダに実体を書いていたが、値が各 .cpp の obj に個別に焼き込まれるため、
//   ビルドシステムが一部の obj の再コンパイルを取りこぼすと「スプラッシュは v1.4.2 なのに
//   Updater は自分を 1.2.1 と思っている」exe ができ、自動更新が無限ループする事故が起きた
//   （v1.2.2〜v1.4.2 の配布物で実際に発生）。単一 TU なら版のズレは起こらず、
//   installer/build.ps1 が --write-version でパッケージ前に版の一致を検証する。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
extern const char* const kEngineVersion;

// 自動アップデートの取得元 GitHub リポジトリ。
extern const char* const kUpdateRepoOwner;
extern const char* const kUpdateRepoName;

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
extern const char* const kWhatsNewTitle;
extern const char* const kWhatsNewBody;
} // namespace dx12e
