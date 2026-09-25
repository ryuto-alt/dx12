#pragma once

// ===== オーディオ・ミキサー窓 =====
// 鳴っている音を「見て」調整するための独立フローティング窓（メニュー「ツール > オーディオミキサー」）。
//   ・バスごとのストリップ: メーター（RMS の帯 + ピークの線、dBFS）/ フェーダー / ミュート / ローパス
//   ・スナップショット: 定義済みの一覧から選んで秒数を決めて切り替え（遷移の進み具合も出す）
//   ・リバーブ: 今効いているゾーン・響きの量・残響時間
//   ・ボイス一覧: 音名 / バス / 優先度 / 聞こえ具合(dB) / 距離 / 遮蔽 / 実 or 仮想（理由）/ 位置
// 値は AudioSystem から毎フレーム読むだけで、エディタ側に状態を持たない（MCP audio_state と同じ出所）。
// フェーダー等の操作は AudioSystem のバス設定を直接変える＝ Play 中の Lua の setBusVolume と同じ値。
// シーンには保存されない（ミックスはゲームのスクリプトかオプション画面が決めるもの）。

namespace dx12e
{

class AudioSystem;
class EditorContext;

// ctx.showAudioMixer が false なら何もしない。EditorLayer が毎フレーム 1 回呼ぶ。
void RenderAudioMixerPanel(AudioSystem* audio, EditorContext& ctx);

} // namespace dx12e
