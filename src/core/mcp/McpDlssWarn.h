#pragma once
// DLSS 縮退の警告を出すかの判定（Application に依存しない純関数）。
#include <string>

namespace mcpdlss {

// 縮退の理由 reason を警告に出すべきか。last は最後に出した理由（空 = まだ出していない）。
// 理由が変わらない限り（dlss → off → dlss を繰り返しても）1 回だけ。毎フレーム呼ばれるので、一致のときは確保しない。
inline bool ShouldLogFallback(std::string& last, const std::string& reason)
{
    if (last == reason) return false;
    last = reason;
    return true;
}

} // namespace mcpdlss
