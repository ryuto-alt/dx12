#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dx12e::http
{

// 同期 HTTPS GET（Updater.cpp の HttpsFetch を汎用化したもの）。ブロッキングなので
// ワーカースレッドから呼ぶこと(UIスレッドで呼ぶとフリーズする)。リダイレクトは WinHTTP 既定で追従。
// 成功(HTTPステータス200)なら out にボディを書いて true。失敗/cancel成立時は false。
// progress: 受信バイト数/総バイト数(Content-Length、不明なら0)を逐次通知。
// cancel: 呼び出し側が true にセットしたら受信ループを中断する(パネル破棄時のjthread終了用)。
bool Get(const std::wstring& url, std::vector<uint8_t>& out,
         const std::function<void(uint64_t done, uint64_t total)>* progress = nullptr,
         const std::atomic<bool>* cancel = nullptr);

} // namespace dx12e::http
