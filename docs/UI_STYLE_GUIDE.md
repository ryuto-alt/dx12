# UIスタイルガイド — ジャンル別デザイン語彙とエンジン機能の対応表

実在ゲームのUI観察(Game UI Database / Interface In Game / GDC講演 / 開発者インタビュー)を、
このエンジンの UI 機能(`docs/SCRIPTING.md` のゲーム内UI節)へ落とすためのリファレンス。
AI/人間がシーン JSON + Lua で「そのジャンルらしい」UIを組むときにここを参照する。

## 鉄則(ジャンル以前の品質差)

1. **画面ごとにレイアウト文法(骨格)を変える。** 色・フォントは通しで統一し、骨格は
   シネマティック中央型 / 全画面ページ型(罫線行リスト) / コーナークラスタ型 /
   左カラム縦メニュー型 / モーダル箱型 から画面ごとに選ぶ。モーダル箱型は1画面まで。
   全画面が同じレシピだと色が揃っていても「全部同じ画面」に見える。
2. **装飾ギミックは配分する。** skew・グロス・ブラケット等を全画面同時に使わない。
3. **動きは複数チャネルで重ねる**(Juice)。被ダメ=色フラッシュ+シェイク+SE。単発は安っぽい。
4. **linearイージング禁止**(装飾的な動きには必ず ease)。全要素が同じ duration/easing で
   動くのも禁止 — 重要度で強弱をつける。
5. **サイズ・太さで階層を作る。** 数字は大きく、ラベルは小さく細く。等サイズの羅列はUIではなく表。

## ジャンル別語彙 → エンジン機能

| ジャンル(代表作) | 形 | 色の文法 | 字 | エンジンでの作り方 |
|---|---|---|---|---|
| スタイリッシュJRPG(ペルソナ5/FE) | 斜め帯・平行四辺形・はんこ(1文字反転) | 黒#0d0d0d+主色1色(赤#d92323)+白。サブ色は数値のみ | 太字+サイズ継ぎ接ぎ・字間不均一 | `skewX`±8〜15 / `rotation`±3〜8 / 白直線=細長Image / `letterSpacing`大きめ / 出現=slideIn+`expo` |
| 西洋AAAミニマル(GoW/TLOU/Horizon) | 円・リング・細線 | 低彩度。赤=否定専用。白細線=インタラクト | 装飾なしサンセリフ、可変サイズ | `shape=リング`ゲージ / `outlineWidth`1-2の細枠 / HUDは非戦闘時 `hideUi`(コンテキストフェード) |
| SF/サイバーパンク(2077/Destiny/NieR) | 角丸なし矩形ブロック・四隅固定・放射線 | 2077=赤+青(弾数)、NieR=ベージュ単色 | 角ばったサンセリフ(Rajdhani系)。意味のない装飾英字 | `outlineStyle=ブラケット` / `cornerRadius=0` / `uvScroll`走査線 / `charAnim=ジッター`(グリッチ) / 四隅アンカー配置 |
| ホラー(RE/SH/Dead Space) | 心電図・三日月メーター・グリッド枠 | 状態色 緑→黄→橙→赤。全体は暗く低情報 | 小文字ミニマル | HP色を`setUiColor`で段階変化 / 低HP=`loopAnim=点滅`+赤ビネット / UIを極小化(非表示が正解) |
| かわいい/コージー(あつ森/Stardew/Fall Guys) | 鋭角ゼロ。豆型・丸ボタン | パステル低彩度(#9dffb0系)or木目茶。白背景 | 丸ゴシック太字(fontPathで丸font指定) | `cornerRadius`大(高さの40-50%) / `outlineWidth`3-4の太縁 / 出現=`bounce`/`elastic` / `charAnim=ウェーブ` |
| レトロ/ピクセル(Shovel Knight/Undertale) | 直角のみ。白罫線+黒地 | 少色数(4-6色)。差し色1点(SOUL赤) | ピクセルフォント(fontPath)。キャラごとにfont切替 | `cornerRadius=0`+`outlineWidth`2-3 / グラデ・影・角丸を使わない(禁欲が正解) / `typewriterSpeed`必須 |
| モバイルガチャ(FGO/原神/ウマ娘) | 金トリム枠(レア度=装飾密度)・光柱 | 白基調+レア度色(金/紫/虹) | アイコン主導。数値に"+" | `gradientScrollSpeed`グロス / `gradientDir=縦`金グラデ / レインボー=`charAnim=3` / バッジ=`shape=ダイヤ` / 出現=`elastic`+スケール |
| レーシング/スポーツ(Forza/GT/FC) | 三角グリッド・半透明ガラスパネル | 黒+白+ピッチ色1色 | 幾何学的サンセリフ・イタリック(=skew) | 半透明黒パネル(α0.5-0.7)+細outline / `skewX`で速度感 / 数字大きく`countTo` |
| 格闘(SF6/GGST) | セグメントゲージ・円形バッジ | ゲージ緑→黄→赤(残量で変化) | グラフィティ風・コンボ数字は巨大 | `segments`でドライブゲージ / `shape=楕円`バッジ / コンボ=`countTo`+`punch` / ゲージ色=`fill`+`setUiColor`連動 |
| ファンタジーRPG(Elden Ring/D4/FF14) | ダイヤ=一時バフ・六角ノード・細金枠 | 暗色+羊皮紙/金。青=マジック黄=レア | セリフ系(fontPath)・小さめ | `shape=ダイヤ`(バフ)/`shape=六角形`(スキルツリー) / `outlineColor`金#c0a060 / `gradientDir=放射`でオーブ |

## モーション相場表(リサーチ実測)

| 用途 | duration | easing | エンジンでの書き方 |
|---|---|---|---|
| ボタン押下感 | 0.12s拡大+0.09s戻し | out→back | `uifx.punch(e.source)` |
| パネル/メニュー遷移 | 0.2〜0.4s | out/expo(overshootなし) | `uifx.slideInLeft` / showAnim+`expo` |
| ステージャー間隔 | 0.05〜0.10s(要素多いほど詰める) | — | `uifx.stagger(items, 0.07, uifx.slideInLeft)` / showDelay |
| ゲージ増減 | 0.3s前後 | out | `tweenUi{fill=}` / `uifx.fillTo` |
| ゴーストバー(遅延削れ) | 本体0.08s+遅れ0.35s後0.45s | out | `uifx.damageBar(front, ghost, v)` |
| 数字ロール | 0.6〜0.8s | expo(最初速く後半減速) | `uifx.countTo(e, v, 0.8, "%06d")` |
| 祝祭(結果/ガチャ/実績) | 0.8〜1.2s | elastic/bounce | `uifx.bounceIn` / `tweenUi{easing="elastic"}` |
| 被ダメ | フラッシュ0.1s+シェイク0.4s | — | `uifx.hit(hpBar)` |
| クールダウン | リアルタイム減 | linear可(進捗表示は例外) | `shape=リング`+`fill`、完了時 `uifx.heartbeat` |
| 画面ワイプ | 0.3〜0.6s | inOut | `clipChildren`+子を`tweenUi{dx=}` / transitionToScene |

- **SE同期**: 視覚のピークと音のアタックを合わせる。`tweenUi{onComplete=}`で完了時に鳴らす。
- **方向の意味**: 階層を下る=右/下から入場、戻る=逆方向。空間の一貫性を守る。
- **連打対策**: ハンドラ先頭で `scene:stopUiTweens(e.source)` してから掛け直す。

## エンジン語彙チートシート(何がどの機能でできるか)

- **円形ゲージ/クールダウン**: `UIImage.shape=2(リング)` + `fillAmount`(+`fillOrigin`)。
  矩形アイコンのクールダウンスワイプは `fillDir=4(放射)` が矩形にもそのまま効く
- **丸アイコン/バッジ/バフ枠**: `shape=1(楕円)/3(ダイヤ)/4(六角形)` — テクスチャは形で切り抜かれる
- **分割ゲージ(スタミナ/弾数)**: `segments` + `segmentGap/Color`
- **動くパターン背景(ストライプ/警告帯)**: タイルテクスチャ + `uvMax=(8,1)` + `uvScroll=(0.5,0)`
- **SF照準枠**: `outlineStyle=2(ブラケット)` + `outlineDash`=腕長。点線枠は `outlineStyle=1`
- **金色タイトル**: `UIText.gradientDir=2` + `gradientColor2` 暗金。にぎやかし文字=`charAnim`
- **ワイプ公開/マーキー**: 親に `clipChildren=true`、子を `tweenUi{dx/dy}` で動かす
- **リスト/グリッド**: 親に `UILayout`(VBox/HBox/Grid)。子は手動配置不要。スクロールは
  UIScrollView→子コンテンツノードに UILayout
- **グロス光沢(ガチャボタン)**: `gradientDir>0` + `gradientScrollSpeed`(gradientColor2は明るい色に)
- **スポットライト/オーブ**: `gradientDir=4(放射)`
- **タイトルの字間**: `letterSpacing` 4〜12(ミニマル系はさらに広く)

## 情報源

Game UI Database / Interface In Game / GDC "Juice It or Lose It" / GDC "Tenacious Design and
the Interface of Destiny" / ATLUS CEDEC(ペルソナ5) / SF6公式UIコラム / 各作品のUI分析記事
(2026-07 リサーチ)。
