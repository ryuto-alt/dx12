#pragma once

#include <string>

namespace dx12e
{

// 枠なしスプラッシュスクリーン（Unity/UE 風の起動画面）。
// メインウィンドウが出る前に、ロゴ + 進行状況 + アニメーションバーを表示する。
//
// 専用スレッドで自前のメッセージポンプを回すため、メインスレッドが重い初期化
// （D3D12 デバイス / シェーダ / PSO / アセット）でブロックしていても
// 再描画・アニメーションが止まらない＝「白い画面で固まる」が起きない。
//
// 全 API は多重呼び出し・未表示時呼び出しに安全（no-op）。
class SplashScreen
{
public:
    // スプラッシュを表示する（すでに表示中なら何もしない）。
    //   titleUtf8   : 大見出し（例 "DX12 Engine"）
    //   versionUtf8 : 右下に小さく出す版数（例 "v0.7.0"。空で非表示）
    //   logoPathUtf8: ロゴ PNG の絶対パス（空 or 読めない場合はテキストのみ）
    static void Show(const std::string& titleUtf8,
                     const std::string& versionUtf8,
                     const std::string& logoPathUtf8);

    // 進行状況の 1 行を差し替える（UTF-8。表示していなければ no-op）
    static void SetStatus(const std::string& statusUtf8);

    // 閉じてスレッドを合流する（未表示なら no-op）
    static void Close();
};

} // namespace dx12e
