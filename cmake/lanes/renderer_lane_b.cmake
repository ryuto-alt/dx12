# renderer のレーン B 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Renderer PRIVATE
    vfx/VfxAsset.cpp      # W1-BD4: .uvfx の読み書き・検証・命令列への変換
    vfx/VfxCurl.cpp       # W1-BD4: 64^3 カール場の生成と CPU 参照
    vfx/VfxRuntime.cpp    # W1-BD4: ホストとの口・MCP からの要求
    vfx/VfxSystem.cpp     # W1-BD4: GPU リソースとシミュレーション・描画の記録
    vfx/VfxFeature.cpp    # W1-BD4: IRenderFeature（UnoFX）
)
