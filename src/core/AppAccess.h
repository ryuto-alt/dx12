#pragma once
// ===========================================================================
// AppAccess（W05-01b）: Application の private に触れる関数の入口。
//   Application.h は W7 まで凍結なので、private への出入り口はここへ static で足す。
//   例: static void SetQualityPreset(Application&, QualityPreset);
//   include するのは使う TU だけ（Application.h からは include しない）。
//   各レーンは自分のブロックだけに追記し、定義は自分の .cpp に書く。
// ===========================================================================
#include "core/Application.h"

namespace dx12e
{
struct AppAccess
{
    // ==== lane-A begin ====
    // ==== lane-A end ====

    // ==== lane-B begin ====
    // ==== lane-B end ====

    // ==== lane-C begin ====
    // ==== lane-C end ====

    // ==== lane-D begin ====
    // ==== lane-D end ====
};
} // namespace dx12e
