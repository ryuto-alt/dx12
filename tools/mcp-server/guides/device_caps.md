# GPU 機能(caps)とアップスケーラの状態
> 使えるハードウェア機能(Agility・SM6.9・Enhanced Barriers・DXR・SER・OMM など)は get_device_caps、DLSS が効いているかは get_upscaler で見る。

```
dx12_call {name:"get_device_caps"}   # 読み取り専用。キーは dx12_tool_describe {name:"get_device_caps"} と実際の結果で確認する
dx12_call {name:"get_upscaler"}      # mode(頼んだ値) と effective(今フレームの実際) / renderResolution / outputResolution / dlss{...}
```

> 暫定(W05-04 未統合): `get_device_caps` の結果のキー名は、この段階では確定していない。キーを推測して読まず、結果の JSON をそのまま見る。キー名の一覧は W05-04 の統合後に、ハンドラ/マニフェストと突き合わせて書き直す(M5 の確認項目)。

## 読み方
- caps は**読むだけ**。値によって描画の経路が自動で変わることは(この段階では)ない。
- シェーダーモデルは Agility SDK が効いていれば 6.9。ドライバが古いと 6.8 にとどまる(エンジンのログに caps の 1 行要約が出る)。
- `get_upscaler`: `mode:"dlss"` を頼んだのに `effective:"taa"` なら、DLSS が使えず TAA に落ちている。**理由は `dlss.fallbackReason`**(DLSS の DLL が無い、GPU が非対応、ドライバが古い、など)。直す前に理由を読む。落ちても画は出る(TAA)。
- 性能の比較では、DLSS 列 / TAAU 列 / TAA native 列を混ぜない。`set_upscaler` で揃えてから測る。
