#pragma once
// ===========================================================================
// ハードウェア窓（editor/panels/HardwarePanel）と Application の受け渡し（POD + 文字列/配列だけ）。
// パネルは要求（requestFlash / requestToolchainInstall）と選択状態だけ書き、Application
// （core/mcp/ApplicationMcpHardware.cpp の ServiceHardwareUi）がスナップショットを毎フレーム書き戻す。
// editor ライブラリが hardware ライブラリに依存しないよう、型は std のみ。
// ===========================================================================
#include <string>
#include <vector>

namespace dx12e::hwui
{

struct DeviceRow
{
    std::string name;
    std::string status;      // disconnected / opening / waiting_hello / ready / error（HwStatusName）
    std::string port;
    std::string board;
    std::string lastError;
    bool        serial = true;   // transport が serial
    bool        isVirtual = false;
};

struct PortRow
{
    std::string port;
    std::string friendlyName;
    std::string vid, pid;
    std::string guessedBoard;
    bool        knownVid = false;   // 1A86 / 10C4 / 0403 / 2341 / 2A03 / 303A
    std::string usedBy;             // 接続中のデバイス名（空 = 使われていない）
};

struct UiState
{
    // ---- 入力（パネル → アプリ）----
    bool        requestFlash = false;
    std::string reqSketch;          // 相対（プロジェクト基準）か絶対
    std::string reqDevice;
    std::string reqPort;
    std::string reqFqbn;
    bool        requestToolchainInstall = false;

    // パネルの選択状態（窓を閉じても保つ）
    std::string selSketch, selPort, selDevice;
    int         selBoard = 0;           // パネル内のボード表の添字
    bool        boardTouched = false;   // ユーザーが手で選んだら true（ポート変更の推定で上書きしない）
    std::string lastPortForBoard;

    // ---- スナップショット（アプリ → パネル）----
    bool        projectOpen = false;
    std::vector<DeviceRow>   devices;
    std::vector<PortRow>     ports;
    std::vector<std::string> sketches;   // <project>/firmware/<名前>/<名前>.ino の名前

    // 道具箱（arduino-cli + コア）
    std::string tcState = "idle";        // idle / downloading / installing_cores / ready / failed
    float       tcProgress = 0.0f;       // downloading のとき 0..1
    bool        tcReady = false;
    bool        tcRunning = false;
    std::string tcCli, tcError;
    std::vector<std::string> tcLog;

    // 書き込み
    std::string flashState = "idle";     // idle / installing_core / compiling / uploading / reconnecting / done / failed
    bool        flashRunning = false;
    float       flashElapsedSec = 0.0f;
    std::string flashError;
    std::string flashDevice, flashPort;
    std::vector<std::string> flashLog;
    std::string requestError;            // 直近の「ボードに書き込む」が開始できなかった理由

    // 自動で窓を開いたときの案内（ボードが見つかったが hello が来ない）
    std::string promptMessage;
    std::string promptPort;
};

} // namespace dx12e::hwui
