# scene のレーン B 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Scene PRIVATE
    ComponentSerializerRegistry.cpp
    VfxComponentSerializer.cpp   # W1-BD4: VfxComponent（"vfx"）の保存 / 読み込み / Undo
)
