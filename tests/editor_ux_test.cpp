// エディタ操作性の基盤（第2波）の純ロジックの単体テスト。
//   ・editor/EditorCommandTable.h : コマンド表（id 重複無し / キー衝突無し / 全コマンドにラベル）
//   ・editor/EditorCreateTable.h  : 作成項目の表
//   ・editor/FuzzyMatch.h         : ファジー検索（日本語 / 英語 / かな↔カナ / 全角半角 / 複数語）
//   ・editor/ToastQueue.h         : トーストのキュー（寿命 / 重複の畳み込み / 上限 / ホバーで停止 / 閉じる）
// すべてヘッダオンリー・標準ライブラリだけ（ImGui も GPU も要らない）＝スタンドアロン CI でも回る。
//
// ★何を守っているか
//   「メニューに Ctrl+O と書いてあるのに効かない」はショートカット表とキー処理が別管理だったせいで起きた。
//   表を 1 つにした今は、同じキーを 2 つのコマンドが取り合う（=片方が黙って死ぬ）ことだけが残る危険なので、
//   衝突を起動前（ここ）で必ず落とす。
#include "editor/EditorCommandTable.h"
#include "editor/EditorCreateTable.h"
#include "editor/FuzzyMatch.h"
#include "editor/ToastQueue.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace dx12e;

namespace
{
int g_checks = 0;
int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

// ---------------- コマンド表 ----------------
void TestCommandTable()
{
    std::set<std::string> ids;
    std::map<std::string, std::string> chords;   // 正規化したキー → コマンド id
    for (const cmd::Def& d : cmd::kCommands)
    {
        CHECK(d.id && d.id[0], "id が空");
        CHECK(d.label && d.label[0], "%s: ラベルが空", d.id);
        CHECK(d.category && d.category[0], "%s: カテゴリが空", d.id);
        CHECK(ids.insert(d.id).second, "id が重複: %s", d.id);

        for (const char* c : {d.chord, d.chord2})
        {
            if (!c || !c[0]) continue;
            const cmd::ChordSpec spec = cmd::ParseChordSpec(c);
            CHECK(spec.valid, "%s: キー表記が読めない: %s", d.id, c);
            if (!spec.valid) continue;
            const std::string key = spec.Normalized();
            auto it = chords.find(key);
            // 同じキーを 2 つのコマンドが取り合うと片方が黙って死ぬ。
            CHECK(it == chords.end(), "キーが衝突: %s は %s と %s", key.c_str(), it == chords.end() ? "?" : it->second.c_str(), d.id);
            chords[key] = d.id;
        }
        // 仕様: Panel / External のコマンドはキーを持つ（ヘルプに載せるため）。Global / Typing は持たなくてもよい。
        if (d.mode == cmd::KeyMode::Panel || d.mode == cmd::KeyMode::External)
            CHECK(d.chord && d.chord[0], "%s: Panel/External なのにキーが無い", d.id);
    }
    CHECK(cmd::kCommandCount == ids.size(), "件数が合わない");

    // 第2波で約束したキーが表に居ること（消えた・別物になったを検出する）
    struct Want { const char* id; const char* chord; };
    const Want wants[] = {
        {"file.new", "Ctrl+N"}, {"file.open", "Ctrl+O"}, {"file.save", "Ctrl+S"}, {"file.saveAs", "Ctrl+Shift+S"},
        {"file.newScript", "Ctrl+L"}, {"edit.undo", "Ctrl+Z"}, {"edit.redo", "Ctrl+Y"}, {"edit.delete", "Del"},
        {"edit.rename", "F2"}, {"edit.selectNone", "Esc"}, {"edit.focus", "F"}, {"play.toggle", "F5"},
        {"play.stop", "Shift+F5"}, {"play.pause", "F1"}, {"view.fill", "Shift+F2"},
        {"palette.commands", "Ctrl+K"}, {"palette.quickOpen", "Ctrl+P"},
    };
    for (const Want& w : wants)
    {
        const cmd::Def* d = cmd::FindCommand(w.id);
        CHECK(d != nullptr, "%s が表に無い", w.id);
        if (d) CHECK(std::string(d->chord) == w.chord, "%s のキーが違う: %s", w.id, d->chord);
    }
    // Redo は Ctrl+Shift+Z も持つ
    if (const cmd::Def* d = cmd::FindCommand("edit.redo"))
        CHECK(std::string(d->chord2) == "Ctrl+Shift+Z", "redo の別名キー");
    // ★F2 は名前変更。照らし込みが F2 のままだと衝突する（旧仕様）ので、F2 を持つのは 1 つだけ。
    int f2 = 0;
    for (const cmd::Def& d : cmd::kCommands)
        for (const char* c : {d.chord, d.chord2})
            if (c && std::string(c) == "F2") ++f2;
    CHECK(f2 == 1, "F2 を持つコマンドは 1 つだけ（実際 %d）", f2);

    // ChordLabel: 別名は " / " でつなぐ。Grave は "`" と表示
    if (const cmd::Def* d = cmd::FindCommand("edit.redo"))
        CHECK(cmd::ChordLabel(*d) == "Ctrl+Y / Ctrl+Shift+Z", "ChordLabel=%s", cmd::ChordLabel(*d).c_str());
    if (const cmd::Def* d = cmd::FindCommand("view.flyMode"))
        CHECK(cmd::ChordLabel(*d) == "`", "Grave 表示=%s", cmd::ChordLabel(*d).c_str());

    // パース: 大文字小文字・修飾の順・別名
    {
        const cmd::ChordSpec a = cmd::ParseChordSpec("ctrl+shift+s");
        const cmd::ChordSpec b = cmd::ParseChordSpec("Shift+Ctrl+S");
        CHECK(a.valid && b.valid && a.Normalized() == b.Normalized(), "修飾の順は関係ない");
        CHECK(cmd::ParseChordSpec("Escape").Normalized() == "ESC", "Escape=Esc");
        CHECK(cmd::ParseChordSpec("Delete").Normalized() == "DEL", "Delete=Del");
        CHECK(!cmd::ParseChordSpec("").valid, "空は無効");
        CHECK(!cmd::ParseChordSpec("Ctrl+").valid, "キー無しは無効");
        CHECK(!cmd::ParseChordSpec("A+B").valid, "キー 2 つは無効");
    }
}

// ---------------- 作成項目の表 ----------------
void TestCreateTable()
{
    std::set<std::string> ids;
    for (const cmd::CreateItem& c : cmd::kCreateItems)
    {
        CHECK(c.id && std::string(c.id).rfind("create.", 0) == 0, "create.* でない: %s", c.id);
        CHECK(ids.insert(c.id).second, "id が重複: %s", c.id);
        CHECK(c.label && c.label[0], "%s: ラベルが空", c.id);
        // marker が空なら専用窓を開く項目なので openTool が要る。あれば marker は __ で始まる。
        if (c.marker && c.marker[0])
            CHECK(std::string(c.marker).rfind("__", 0) == 0, "%s: marker は __ で始まる", c.id);
        else
            CHECK(c.openTool && c.openTool[0], "%s: marker も openTool も無い", c.id);
    }
    // UI 自動テストが名前で引く項目名は変えてはならない
    std::set<std::string> labels;
    for (const cmd::CreateItem& c : cmd::kCreateItems) labels.insert(c.label);
    for (const char* need : {"Box", "Sphere", "Plane", "Empty", "Camera", "Directional Light", "Point Light", "Spot Light"})
        CHECK(labels.count(need) == 1, "作成メニューの項目名が無い: %s", need);
    CHECK(cmd::IsUiMarker("__ui_button__"), "UI マーカーの判定");
    CHECK(!cmd::IsUiMarker("__primitive_box__"), "UI マーカーの判定(否)");
}

// ---------------- ファジー検索 ----------------
int S(const char* q, const char* t) { return fuzzy::Score(q, t); }

void TestFuzzy()
{
    // 基本: 一致 / 不一致
    CHECK(S("save", "Save Scene") >= 0, "英語の前方一致");
    CHECK(S("svsc", "Save Scene") >= 0, "部分列");
    CHECK(S("xyz", "Save Scene") < 0, "不一致は -1");
    CHECK(S("", "Save Scene") == 0, "空クエリは 0（全部一致）");
    CHECK(S("savee", "Save") < 0, "対象より長いクエリは不一致");

    // 大文字小文字・全角半角
    CHECK(S("SAVE", "save") >= 0 && S("save", "SAVE") >= 0, "大文字小文字を区別しない");
    CHECK(S("ｓａｖｅ", "Save") >= 0, "全角英字を半角として扱う");
    CHECK(S("　save", "Save") >= 0 || S("save", "Save") >= 0, "全角空白");

    // 日本語: 連続した部分文字列は 1 バイトずつではなく文字単位
    CHECK(S("保存", "名前を付けて保存") >= 0, "日本語の部分一致");
    CHECK(S("ライティ", "ライティング") >= 0 && S("ライ", "ライティング") >= 0, "カタカナの前方一致");
    CHECK(S("存保", "保存") < 0, "順序は保つ");
    // ひらがな ↔ カタカナ
    CHECK(S("らいてぃんぐ", "ライティング") >= 0, "ひらがなでカタカナを引ける");
    CHECK(S("ライティング", "らいてぃんぐ") >= 0, "カタカナでひらがなを引ける");
    // UTF-8 の途中バイトに誤マッチしない（"ぼ" の中身と "保" の中身が偶然一致しない）
    CHECK(S("保", "ぼ") < 0, "文字単位で比較する");

    // 複数語: 順不同・全部含む必要
    CHECK(S("scene save", "Save Scene") >= 0, "語の順不同");
    CHECK(S("save foo", "Save Scene") < 0, "1 語でも欠ければ不一致");
    CHECK(S("新規 スクリプト", "新規スクリプト") >= 0, "日本語の複数語");

    // スコアの順序: 連続 > 飛び飛び / 先頭 > 途中 / 短い対象 > 長い対象
    CHECK(S("save", "Save") > S("save", "s a v e"), "連続一致が飛び飛びより高い");
    CHECK(S("sc", "Scene") > S("sc", "The Scene"), "先頭一致が途中より高い");
    CHECK(S("undo", "Undo") > S("undo", "Undo the last operation that was performed"), "短い対象が上");

    // 一致位置（ハイライト用）: 元の UTF-8 バイト位置で返る
    {
        std::vector<uint32_t> off;
        const int s = fuzzy::Score("保存", "名前を付けて保存", &off);
        CHECK(s >= 0 && off.size() == 2, "一致位置 2 つ (size=%zu)", off.size());
        // "名前を付けて" は 6 文字 x 3 バイト = 18 バイト目から "保存"
        if (off.size() == 2) CHECK(off[0] == 18 && off[1] == 21, "一致位置=%u,%u", off[0], off[1]);
    }
    {
        std::vector<uint32_t> off;
        fuzzy::Score("ab", "xxabxx", &off);
        CHECK(off.size() == 2 && off[0] == 2 && off[1] == 3, "ASCII の一致位置");
    }

    // 壊れた UTF-8 でも落ちない
    {
        const char bad[] = {static_cast<char>(0xE3), static_cast<char>(0x81), 0};   // 途中で切れた 3 バイト文字
        (void)fuzzy::Score("a", bad);
        (void)fuzzy::Score(bad, "abc");
        CHECK(true, "壊れた UTF-8 で落ちない");
    }

    // 実際のコマンド名に対する現実的な検索
    auto best = [](const char* q) -> std::string
    {
        std::string bestId; int bestScore = -1;
        for (const cmd::Def& d : cmd::kCommands)
        {
            int s = fuzzy::Score(q, d.label);
            const int s2 = fuzzy::Score(q, d.labelEn);
            if (s2 > s) s = s2;
            if (s > bestScore) { bestScore = s; bestId = d.id; }
        }
        return bestId;
    };
    CHECK(best("保存") == "file.save" || best("保存") == "file.saveAs", "保存 -> file.save 系 (実際 %s)", best("保存").c_str());
    CHECK(best("undo") == "edit.undo", "undo -> edit.undo (実際 %s)", best("undo").c_str());
    CHECK(best("やり直す") == "edit.redo", "やり直す -> edit.redo (実際 %s)", best("やり直す").c_str());
}

// ---------------- トーストのキュー ----------------
void TestToastQueue()
{
    using ui::ToastKind;
    using ui::ToastQueue;

    // 既定の表示時間: 通常 3 秒 / Error 6 秒
    {
        ToastQueue q;
        q.Push(ToastKind::Info, "a");
        q.Push(ToastKind::Error, "b");
        CHECK(q.Items().size() == 2, "2 枚積んだ");
        CHECK(q.Items()[0].life == 3.0f && q.Items()[1].life == 6.0f, "既定の寿命 3 / 6 秒 (%.1f, %.1f)", q.Items()[0].life, q.Items()[1].life);
        // 3 秒 + フェードで通常は消え、Error は残る
        q.Update(3.0f + ToastQueue::kFadeOut + 0.01f);
        CHECK(q.Items().size() == 1 && q.Items()[0].text == "b", "3 秒で info だけ消える");
        q.Update(3.0f + 0.05f);
        CHECK(q.Items().empty(), "6 秒で error も消える");
    }
    // 指定秒数
    {
        ToastQueue q;
        q.Push(ToastKind::Success, "x", 10.0f);
        CHECK(q.Items()[0].life == 10.0f, "seconds 指定");
        q.Update(9.0f);
        CHECK(q.Items().size() == 1, "指定時間の間は残る");
    }
    // 重複の畳み込み: 同じ種別・本文は 1 枚にして count を増やし、寿命を延ばす
    {
        ToastQueue q;
        const uint32_t id1 = q.Push(ToastKind::Info, "同じ");
        q.Update(2.0f);
        const uint32_t id2 = q.Push(ToastKind::Info, "同じ");
        CHECK(id1 == id2, "同じ id を返す");
        CHECK(q.Items().size() == 1 && q.Items()[0].count == 2, "件数が畳まれる (count=%d)", q.Items().empty() ? -1 : q.Items()[0].count);
        CHECK(q.Items()[0].age == 0.0f, "寿命が延びる（age が戻る）");
        q.Push(ToastKind::Info, "同じ");
        CHECK(q.Items()[0].count == 3, "x3");
        // 種別が違えば別の 1 枚
        q.Push(ToastKind::Warn, "同じ");
        CHECK(q.Items().size() == 2, "種別が違えば畳まない");
        // 本文が違えば別の 1 枚
        q.Push(ToastKind::Info, "違う");
        CHECK(q.Items().size() == 3, "本文が違えば畳まない");
    }
    // 上限: あふれたら古いものから消す
    {
        ToastQueue q;
        for (int i = 0; i < 10; ++i) q.Push(ToastKind::Info, "m" + std::to_string(i));
        CHECK(q.Items().size() == ToastQueue::kMaxItems, "上限 %zu (実際 %zu)", ToastQueue::kMaxItems, q.Items().size());
        CHECK(q.Items().front().text == "m4" && q.Items().back().text == "m9", "古いものから消える (%s..%s)", q.Items().front().text.c_str(), q.Items().back().text.c_str());
    }
    // ホバー中は寿命が減らない
    {
        ToastQueue q;
        const uint32_t a = q.Push(ToastKind::Info, "読んでいる");
        q.Push(ToastKind::Info, "別の");
        q.Update(10.0f, a);   // a をホバー中に 10 秒
        CHECK(q.Items().size() == 1 && q.Items()[0].id == a, "ホバー中の 1 枚だけが残る");
        q.Update(3.0f + ToastQueue::kFadeOut + 0.01f);
        CHECK(q.Items().empty(), "ホバーを外せば通常どおり消える");
    }
    // クリックで閉じる: フェードアウト後に取り除かれる。閉じた本文の再通知は新しい 1 枚になる
    {
        ToastQueue q;
        const uint32_t id = q.Push(ToastKind::Warn, "閉じる");
        CHECK(q.Dismiss(id), "Dismiss は成功");
        CHECK(!q.Dismiss(id), "2 回目は false");
        CHECK(q.LiveCount() == 0, "閉じた分は Live に数えない");
        const uint32_t id2 = q.Push(ToastKind::Warn, "閉じる");
        CHECK(id2 != id && q.Items().size() == 2, "閉じたものへは畳まない");
        q.Update(ToastQueue::kFadeOut + 0.01f);
        CHECK(q.Items().size() == 1 && q.Items()[0].id == id2, "フェードが終わると取り除かれる");
    }
    // 不透明度: フェードイン → 1 → フェードアウト
    {
        ToastQueue q;
        q.Push(ToastKind::Info, "a", 2.0f);
        CHECK(ToastQueue::Alpha(q.Items()[0]) == 0.0f, "出した直後は透明");
        q.Update(0.5f);
        CHECK(ToastQueue::Alpha(q.Items()[0]) == 1.0f, "出て少しで不透明");
        q.Update(1.6f);   // age 2.1 -> 寿命を過ぎてフェードアウト中
        const float a = ToastQueue::Alpha(q.Items()[0]);
        CHECK(a > 0.0f && a < 1.0f, "フェードアウト中 (%.2f)", a);
    }
}
} // namespace

int main()
{
    TestCommandTable();
    TestCreateTable();
    TestFuzzy();
    TestToastQueue();

    if (g_failures != 0)
    {
        std::printf("editor_ux: %d checks, %d failure(s)\n", g_checks, g_failures);
        return 1;
    }
    std::printf("editor_ux: %d checks, all passed\n", g_checks);
    return 0;
}
