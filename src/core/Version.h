#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.4.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.4.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.4.0: 2Dゲーム大型対応 — OGG再生 + スプライトアニメ + スケルタルアニメ修正\n"
    "\n"
    "・OGG Vorbis 対応: BGM/SFX で .ogg が再生可能に(stb_vorbis)\n"
    "・Sprite2D フリップブックアニメ(animFrames/animFps/animCols)と\n"
    "  UVスクロール(scrollU/V)を追加。Editor中もプレビュー再生\n"
    "・スケルタルアニメが正射カメラで静止する重大バグを修正\n"
    "  (ボーン行列のGPUアップロードがシャドウパス依存だった)\n"
    "・アニメ再生速度: entity:setAnimSpeed / dx12_play_anim speed\n"
    "・audio:playBGM/playSFX の loop 引数が省略可能に\n"
    "・起動時に前回プロジェクトを自動復元(アセット読み込み失敗を解消)\n"
    "・MCP: dx12_open_project 追加、create_entity の position 不具合修正\n";
} // namespace dx12e
