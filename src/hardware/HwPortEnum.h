#pragma once
// COM ポート一覧（SetupAPI）。VID/PID と推定ボード名つき。
#include <string>
#include <vector>

namespace dx12e::hw
{

struct HwPortInfo
{
    std::string portName;       // "COM8" / "virtual:radio"
    std::string friendlyName;   // "USB-SERIAL CH340 (COM8)"
    std::string vid;            // 16 進 4 桁（大文字）。不明なら空
    std::string pid;
    std::string guessedBoard;   // 推定。不明なら空
};

// VID/PID（16 進 4 桁・大小どちらでも可）から推定ボード名。不明なら空文字列。
std::string GuessBoardFromVidPid(const std::string& vid, const std::string& pid);

// 接続中の COM ポートを列挙する（COM 番号の自然順）。失敗したら空。
std::vector<HwPortInfo> EnumerateComPorts();

} // namespace dx12e::hw
