#pragma once
// ===========================================================================
// W1-A8 (RM-C3ui): ビューポート帯の「品質: Epic ▾」と Application の品質プリセットをつなぐ口。
//   エディタ（別ライブラリ）は Application を知らないので、Application::Initialize が 3 つの関数を渡す。
//   実体は core/QualityPresetApply.h の GetQualityPreset / SetQualityPreset / CurrentQualityPlan（C7）。
//   EditorContext.h は W1 では A2a の 1 回だけの変更なので、ここへ独立したヘッダとして置く（std のみ・重ヘッダにしない）。
//   未設定（ゲームモード・単体テスト）なら帯は品質のボタンを出さない。
// ===========================================================================
#include <functional>
#include <string>
#include <vector>

namespace dx12e
{

struct QualityPresetUiHook
{
    // 今のプリセット（QualityPresetId の整数値。範囲外は -1）。
    std::function<int()> get;
    // 選ぶ。Undo 1 回分に積む（recordUndo = true 固定）。範囲外・失敗は false。
    std::function<bool(int)> set;
    // 今の caps（DXR / DLSS）で縮退している行の説明（"行名: 説明" 形式。無ければ空）。
    std::function<std::vector<std::string>()> degrades;

    bool Available() const { return static_cast<bool>(get) && static_cast<bool>(set); }
};

// 1 プロセスに 1 つ（Application が 1 つなので）。
inline QualityPresetUiHook& QualityPresetUi()
{
    static QualityPresetUiHook hook;
    return hook;
}

} // namespace dx12e
