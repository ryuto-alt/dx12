// 品質プリセットの設定 / 取得（W1-C1: 口だけ）。
//   C1 は「どのプリセットが選ばれているか」を記録する入口だけを用意する。絵・シーン設定・VFX・アップスケーラへの
//   反映と Undo は C7（RM-C3）で、この関数の中に足す（呼び出し側の署名は変えない）。
//   ★Application.h は凍結なので、状態はここ（Application は 1 つ）に持つ。C7 でプロジェクト設定へ永続化する。
#include "core/QualityPresetApply.h"
#include "core/AppAccess.h"

#include <atomic>

namespace dx12e
{
namespace
{
// 既定は High（従来の見た目。ゴールデンはビット一致のまま）。C7 でプロジェクト設定から初期化する。
std::atomic<u8> g_currentPreset{static_cast<u8>(QualityPresetId::High)};
} // namespace

bool AppAccess::SetQualityPreset(Application& /*app*/, QualityPresetId id, bool /*recordUndo*/)
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(QualityPresetId::Count)) return false;
    g_currentPreset.store(static_cast<u8>(id), std::memory_order_relaxed);
    return true;
}

QualityPresetId AppAccess::GetQualityPreset(const Application& /*app*/)
{
    return static_cast<QualityPresetId>(g_currentPreset.load(std::memory_order_relaxed));
}

bool SetQualityPreset(Application& app, QualityPresetId id, bool recordUndo)
{
    return AppAccess::SetQualityPreset(app, id, recordUndo);
}

QualityPresetId GetQualityPreset(const Application& app)
{
    return AppAccess::GetQualityPreset(app);
}

} // namespace dx12e
