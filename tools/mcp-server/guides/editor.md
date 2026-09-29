# エディタ UI の操作(仮想入力)
> 人の PC 操作を奪わない。`--background` で起動し、`dx12_imgui_*`(仮想入力)だけで操作する。

## 起動(人のカーソルを奪わない唯一の方法)
```
DX12Engine.exe --background --project <プロジェクトのフォルダ> --mcp-port 8850
```
作業ディレクトリは exe のあるフォルダ。窓は画面外・タスクバー非表示・前面化しない。`dx12_doctor` が起動コマンドを案内する。終わったら必ず閉じる(残すと保存確認ダイアログが居座る)。

## 操作
| ツール | 用途 |
|---|---|
| `dx12_imgui_virtual_input {enable:true}` | 仮想入力モードの ON/OFF(`--background` は既に ON) |
| `dx12_imgui_find {...}` | UI 要素(窓・ボタン・メニュー)の位置を探す |
| `dx12_imgui_pointer {...}` | クリック・ドラッグ(座標は**物理クライアント px**。論理 px = 物理 ÷ dpiScale) |
| `dx12_imgui_key {...}` | キー・文字入力 |
| `dx12_imgui_screenshot {path}` | バックバッファ + ImGui を含む撮影(窓が背面でもよい) |

## してはいけないこと
- `SendInput` / `mouse_event` / `SetCursorPos` / `SetForegroundWindow` / computer-use での操作。
- `dx12_mouse_move` / `dx12_key_press` / `dx12_ui_click` でエディタを操作すること(これらは Play 中のゲーム入力用)。
- 実マウスのスクリーンショット確認(通常のスクショ系はゲーム画を返す。ImGui を含むのは `dx12_imgui_screenshot` だけ)。

## 罠
- 合成クリック(Win32)は ImGui に届かない。仮想入力を使う。
- `key_press` はキーが押しっぱなしにスタックすることがある。押したら `key_up`。
- エディタを閉じる時の保存確認/自動保存復元ダイアログは WM_CLOSE が 2 回要る。生成物でシーンを作る作品は「破棄」でよい。
