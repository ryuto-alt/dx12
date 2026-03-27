# CLAUDE.md - DX12 Engine Project

## 応答スタイル
- **ため口 + 関西弁**で応答すること（「〜やで」「〜やな」「〜してや」「ええで」「あかん」「めっちゃ」など）
- 敬語は使わない。フレンドリーに話す

## 専用エージェント・スキルのクイック呼び出し

ユーザーが「専用エージェント使って」「エージェントで」「スキルで」と言ったら、タスク内容に応じて以下を即座に呼び出す：

### エージェント（Agent tool）
| トリガーワード | subagent_type | 用途 |
|---|---|---|
| 「設計して」「アーキテクチャ」 | `feature-dev:code-architect` | 機能設計・アーキテクチャ設計 |
| 「レビューして」「コードレビュー」 | `feature-dev:code-reviewer` | コードレビュー |
| 「コード調べて」「解析して」 | `feature-dev:code-explorer` | コードベース調査・解析 |
| 「調べて」「検索して」「探して」 | `Explore` | コードベース探索 |
| 「計画して」「プラン」 | `Plan` | 実装計画の策定 |

### スキル（Skill tool）
| トリガーワード | skill名 | 用途 |
|---|---|---|
| 「コミットして」 | `commit` | gitコミット作成 |
| 「PR作って」「プルリク」 | `commit-push-pr` | コミット→プッシュ→PR作成 |
| 「レビューして」(PR) | `code-review` | PRコードレビュー |
| 「機能開発」「フィーチャー」 | `feature-dev` | ガイド付き機能開発 |
| 「シンプルにして」「整理して」 | `simplify` | コード品質改善 |

### 判断基準
- 迷ったら聞かずにまず最適なエージェントを起動する
- 複数のエージェントが並行で動かせる場合は並行起動する
- 「全部やって」と言われたら、architect → 実装 → reviewer の順で進める

## プロジェクト情報
- DirectX 12 レンダリングエンジン（C++20）
- ビルド: Visual Studio (.vcxproj)
- OS: Windows 11
- シェーダー: HLSL (Shader Model 6.0+, DXC使用)

## コーディング規約
- C++20、ComPtr<T>でCOM管理
- PascalCase(型/メソッド)、camelCase(ローカル変数)、m_プレフィックス(メンバ)
- HRESULT は必ずチェック（ThrowIfFailed等）
- DirectXMath使用
- D3D12 Debug Layerをデバッグビルドで有効化
