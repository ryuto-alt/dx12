#pragma once
// ============================================================================
// ハードウェア連携の本体（docs/HARDWARE.md）。
//   IO スレッド 1 本が全デバイスの読み取り・パース・心拍・送信・切断検出・再走査を担当し、
//   メインスレッドは BeginFrame() で「最新値」をフレーム用に確定してから読む（フレーム途中で値が変わらない）。
//   メインスレッド API は全部スレッド安全（m_mutex）。
//
//   出力: Set() は溜めるだけ。BeginFrame() / FlushOutputs() が確定し、IO スレッドが
//        「同じチャンネルは最新値だけ・1 フレーム 1 行」で送る（1 回の確定で 128 バイトを超えるときだけ複数行）。
// ============================================================================

#include "hardware/HwConfig.h"
#include "hardware/HwDevice.h"
#include "hardware/HwPortEnum.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dx12e::hw
{

using HwTransportFactory = std::function<std::unique_ptr<IHwTransport>(const std::string& portName, int baud)>;
using HwPortLister       = std::function<std::vector<HwPortInfo>()>;

struct HwSystemOptions
{
    bool scanRealPorts = true;          // false = 実 COM ポートを列挙しない（テスト用）
    int  scanIntervalMs = 1000;         // 再走査・自動再接続の間隔
    int  helloWaitMs   = 3000;          // @hello を待つ時間（その後 ?hello を送ってさらに helloRetryMs）
    int  helloRetryMs  = 1000;
    int  silenceTimeoutMs = 3000;       // 無受信で切断とみなす時間
    HwTransportFactory factory;         // 空 = 既定（"virtual:" は Loopback、それ以外は Serial）
    HwPortLister       lister;          // 空 = EnumerateComPorts()（実ポート部分だけを置き換える。仮想ポートは常に加わる）
};

class HardwareSystem
{
public:
    HardwareSystem();
    ~HardwareSystem();
    HardwareSystem(const HardwareSystem&) = delete;
    HardwareSystem& operator=(const HardwareSystem&) = delete;

    // ---- 起動・停止 ----
    void Initialize(const HwConfig& cfg, const HwSystemOptions& opts = {});
    void Shutdown();   // 出力を !safe してから閉じる。何度呼んでもよい
    bool IsInitialized() const { return m_running.load(); }

    // ---- フレーム ----
    void BeginFrame();       // 出力の確定 → 最新値をスナップショットへ → pressed/released のエッジ計算
    void FlushOutputs();     // 溜めた出力をいま確定する（フレームを待たない MCP の hw_write 用）

    // ---- 読む（BeginFrame で確定した値） ----
    bool   IsConnected(const std::string& dev) const;
    double Get(const std::string& dev, const std::string& ch) const;      // 正規化値 0..1（校正・平滑済み）。無ければ 0
    double GetRaw(const std::string& dev, const std::string& ch) const;
    bool   Down(const std::string& dev, const std::string& ch) const;     // bool: raw!=0（invert なら反転）/ 数値: value>=0.5
    bool   Pressed(const std::string& dev, const std::string& ch) const;
    bool   Released(const std::string& dev, const std::string& ch) const;

    // ---- 書く ----
    // 正規化値 0..1 を出力チャンネルへ。dangerous は maxValue で切る。デバイスが Ready でチャンネルが
    // 宣言済みの out でなければ false。
    bool Set(const std::string& dev, const std::string& ch, double value01);
    void ResetOutputs();     // 全デバイスへ !safe（Play 停止時）

    // ---- 接続 ----
    bool Connect(const std::string& dev, const std::string& port = {});   // 要求を出すだけ（非同期）。自動再接続も再び有効に
    bool Disconnect(const std::string& dev);                              // !safe して閉じる。自動再接続しない

    // ---- 状態 ----
    std::vector<HwDeviceInfo> ListDevices() const;
    bool GetDeviceInfo(const std::string& dev, HwDeviceInfo& out) const;
    std::vector<HwLogEntry> GetLog(const std::string& dev, size_t n = 200) const;
    std::vector<HwActionBinding> GetActionBindings() const;
    HwConfig Config() const;     // いまの設定（校正値の反映済み）。hardware.json へ書き戻す用

    // ---- 仮想デバイス・シミュレーション ----
    // Loopback デバイスを生やす。同名の仮想デバイスが既にあれば null。設定に同名のデバイスがあればそれに仮想ポートを割り当てる。
    std::shared_ptr<HwLoopbackDevice> AddVirtualDevice(const std::string& name, const std::vector<HwChannelDecl>& channels);
    std::shared_ptr<HwLoopbackDevice> GetVirtualDevice(const std::string& name) const;
    // 入力 ch に raw を流し込む。Loopback なら偽ファームの入力として、実機なら上書き値として扱う。
    bool Simulate(const std::string& dev, const std::string& ch, double raw);
    void ClearSimulation(const std::string& dev, const std::string& ch = {});   // ch 空 = 全部

    // ---- 校正 ----
    bool BeginCalibration(const std::string& dev);
    std::map<std::string, HwCalRange> EndCalibration(const std::string& dev);   // サンプルの無い ch は含まない
    // 校正値を設定へ反映（以後の正規化に使う）。Config() で書き戻せる。
    bool SetChannelRange(const std::string& dev, const std::string& ch, double min, double max);

    // ---- サンプル集計（MCP の hw_read） ----
    bool BeginSampling(const std::string& dev);
    std::map<std::string, HwSampleStats> EndSampling(const std::string& dev);

private:
    HwDevice*       FindLocked(const std::string& name);
    const HwDevice* FindLocked(const std::string& name) const;
    HwChannelState*       FindChLocked(HwDevice& d, const std::string& ch);
    const HwChannelState* FindChLocked(const HwDevice& d, const std::string& ch) const;
    HwDevice& AddSlotLocked(const HwDeviceConfig& cfg, bool fromConfig);
    void FlushOutputsLocked();
    void StartThread();

    // IO スレッド
    void IoThreadMain();
    std::vector<HwPortInfo> ListPorts();
    bool ServiceDevice(HwDevice& d, double nowMs, const std::vector<HwPortInfo>& ports, bool scanTick);
    void TryOpen(HwDevice& d, double nowMs, const std::vector<HwPortInfo>& ports, const std::string& explicitPort);
    void CloseDevice(HwDevice& d, const std::string& reason, bool sendSafe, bool isError, double cooldownMs = 0.0);
    bool SendLine(HwDevice& d, const std::string& text);
    void OnLine(HwDevice& d, const std::string& line, double nowMs);
    std::unique_ptr<IHwTransport> MakeTransport(const std::string& portName, int baud, bool dtr = false);
    void PushLogLocked(HwDevice& d, bool outgoing, const std::string& text);
    HwDeviceInfo MakeInfoLocked(const HwDevice& d, double nowMs) const;

    HwSystemOptions m_opts;

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<HwDevice>> m_devices;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    std::map<std::string, double> m_cooldown;   // IO スレッド専用: ポート名 → この時刻まで開かない
};

} // namespace dx12e::hw
