#pragma once
// PostProcessSettings の V2 フィールド（renderer/PostProcessSettings.h の DX12E_POST_FIELDS_V2）を
// 「既定と同じなら保存もハッシュもしない」で扱う X マクロ部品（W05-15）。
// FIELDS は DX12E_POST_FIELDS_V2 と同じ形（B / F / I / V / S の 5 引数）のマクロ名。
// テストではダミーの表と構造体を渡して確かめる。
// ★マクロの内側は固定名の uno_o / uno_d / uno_j を使う（展開は {} で囲んであるので外へ漏れない）。
//   使う側で nlohmann/json.hpp を include しておくこと（SAVE / LOAD のみ）。

// 保存: OBJ のフィールドのうち DEF（既定値の構造体）と違うものだけ、json J へ書く。
#define UNO_V2_SB(f) if (uno_o.f != uno_d.f) uno_j[#f] = uno_o.f;
#define UNO_V2_SF(f) if (uno_o.f != uno_d.f) uno_j[#f] = uno_o.f;
#define UNO_V2_SI(f) if (uno_o.f != uno_d.f) uno_j[#f] = uno_o.f;
#define UNO_V2_SV(f) if (uno_o.f.x != uno_d.f.x || uno_o.f.y != uno_d.f.y || uno_o.f.z != uno_d.f.z) \
                         uno_j[#f] = ::nlohmann::json::array({uno_o.f.x, uno_o.f.y, uno_o.f.z});
#define UNO_V2_SS(f) if (uno_o.f != uno_d.f) uno_j[#f] = uno_o.f;
#define UNO_POST_V2_SAVE(FIELDS, OBJ, DEF, J)                                   \
    {                                                                           \
        const auto& uno_o = (OBJ); const auto& uno_d = (DEF); auto& uno_j = (J); \
        (void)uno_o; (void)uno_d; (void)uno_j; \
        FIELDS(UNO_V2_SB, UNO_V2_SF, UNO_V2_SI, UNO_V2_SV, UNO_V2_SS)           \
    }

// 読み込み: J にあるキーだけ OBJ へ入れる（無ければ OBJ の既定のまま）。
#define UNO_V2_LB(f) uno_o.f = uno_j.value(#f, uno_o.f);
#define UNO_V2_LV(f) if (uno_j.contains(#f) && uno_j[#f].is_array() && uno_j[#f].size() == 3) { \
                         uno_o.f.x = uno_j[#f][0].get<float>(); uno_o.f.y = uno_j[#f][1].get<float>(); \
                         uno_o.f.z = uno_j[#f][2].get<float>(); }
#define UNO_POST_V2_LOAD(FIELDS, OBJ, J)                                        \
    {                                                                           \
        auto& uno_o = (OBJ); const auto& uno_j = (J);                           \
        (void)uno_o; (void)uno_j; \
        FIELDS(UNO_V2_LB, UNO_V2_LB, UNO_V2_LB, UNO_V2_LV, UNO_V2_LB)           \
    }

// 指紋: DEF と違うフィールドだけ、名前と値を MIX(ptr, size) へ渡す。
#define UNO_V2_HB(f) if (uno_o.f != uno_d.f) { uno_mix(#f, sizeof(#f)); uno_mix(&uno_o.f, sizeof(uno_o.f)); }
#define UNO_V2_HV(f) if (uno_o.f.x != uno_d.f.x || uno_o.f.y != uno_d.f.y || uno_o.f.z != uno_d.f.z) \
                         { uno_mix(#f, sizeof(#f)); uno_mix(&uno_o.f.x, sizeof(float)); uno_mix(&uno_o.f.y, sizeof(float)); uno_mix(&uno_o.f.z, sizeof(float)); }
#define UNO_V2_HS(f) if (uno_o.f != uno_d.f) { uno_mix(#f, sizeof(#f)); uno_mix(uno_o.f.data(), uno_o.f.size()); }
#define UNO_POST_V2_HASH(FIELDS, OBJ, DEF, MIXFN)                               \
    {                                                                           \
        const auto& uno_o = (OBJ); const auto& uno_d = (DEF); auto&& uno_mix = (MIXFN); \
        (void)uno_o; (void)uno_d; (void)uno_mix; \
        FIELDS(UNO_V2_HB, UNO_V2_HB, UNO_V2_HB, UNO_V2_HV, UNO_V2_HS)           \
    }
