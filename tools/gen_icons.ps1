# エディタUIアイコン生成 (System.Drawing / GDI+)
# フラットモダン: 角丸スクエア背景 + 白グリフ
Add-Type -AssemblyName System.Drawing

$outDir = Join-Path $PSScriptRoot "..\assets\editor\icons"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$S = 128

function Get-RoundRect([float]$x,[float]$y,[float]$w,[float]$h,[float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Save-Icon($name, $bgHex, [scriptblock]$draw) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::FromArgb(0,0,0,0))

    # 背景の角丸スクエア
    $bg = [System.Drawing.ColorTranslator]::FromHtml($bgHex)
    $bgBrush = New-Object System.Drawing.SolidBrush($bg)
    $path = Get-RoundRect 8 8 ($S-16) ($S-16) 26
    $g.FillPath($bgBrush, $path)

    & $draw $g

    $g.Dispose()
    $file = Join-Path $outDir "$name.png"
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "wrote $file"
}

$white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
$whitePen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 9)
$whitePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$whitePen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
$whitePen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

# --- new project: ドキュメント + 折り返し角 + 緑のプラス ---
Save-Icon "new_project" "#3B82F6" {
    param($g)
    # 書類
    $doc = Get-RoundRect 40 32 48 64 6
    $g.FillPath($white, $doc)
    # 折り返し角(背景色)
    $bg = [System.Drawing.ColorTranslator]::FromHtml("#3B82F6")
    $corner = New-Object System.Drawing.Drawing2D.GraphicsPath
    $corner.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(74,32)),
        (New-Object System.Drawing.PointF(88,46)),
        (New-Object System.Drawing.PointF(74,46))))
    $g.FillPath((New-Object System.Drawing.SolidBrush($bg)), $corner)
    # プラス(緑丸)
    $green = [System.Drawing.ColorTranslator]::FromHtml("#22C55E")
    $g.FillEllipse((New-Object System.Drawing.SolidBrush($green)), 74, 70, 34, 34)
    $pp = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 6)
    $pp.StartCap=[System.Drawing.Drawing2D.LineCap]::Round; $pp.EndCap=[System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pp, 91, 78, 91, 96)
    $g.DrawLine($pp, 82, 87, 100, 87)
}

# --- open project: フォルダ(開) ---
Save-Icon "open_project" "#F59E0B" {
    param($g)
    $folderBack = Get-RoundRect 34 44 60 48 6
    $g.FillPath($white, $folderBack)
    # タブ
    $tab = New-Object System.Drawing.Drawing2D.GraphicsPath
    $tab.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(34,46)),
        (New-Object System.Drawing.PointF(52,46)),
        (New-Object System.Drawing.PointF(60,38)),
        (New-Object System.Drawing.PointF(34,38))))
    $g.FillPath($white, $tab)
    # 開いた前板(薄色)
    $front = New-Object System.Drawing.Drawing2D.GraphicsPath
    $front.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(38,92)),
        (New-Object System.Drawing.PointF(100,92)),
        (New-Object System.Drawing.PointF(90,62)),
        (New-Object System.Drawing.PointF(28,62))))
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,255,237,200))), $front)
}

# --- recent: 時計 ---
Save-Icon "recent" "#64748B" {
    param($g)
    $g.FillEllipse($white, 36, 36, 56, 56)
    $bg = [System.Drawing.ColorTranslator]::FromHtml("#64748B")
    $g.FillEllipse((New-Object System.Drawing.SolidBrush($bg)), 44, 44, 40, 40)
    $hand = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 6)
    $hand.StartCap=[System.Drawing.Drawing2D.LineCap]::Round; $hand.EndCap=[System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($hand, 64, 64, 64, 50)
    $g.DrawLine($hand, 64, 64, 76, 70)
}

# --- save: フロッピー ---
Save-Icon "save" "#14B8A6" {
    param($g)
    $body = Get-RoundRect 36 36 56 56 6
    $g.FillPath($white, $body)
    $bg = [System.Drawing.ColorTranslator]::FromHtml("#14B8A6")
    $bgBrush = New-Object System.Drawing.SolidBrush($bg)
    # 上部スロット
    $g.FillRectangle($bgBrush, 52, 36, 24, 16)
    # 下部ラベル
    $g.FillRectangle($bgBrush, 48, 64, 32, 28)
}

# --- git: ブランチ ---
Save-Icon "git" "#F05133" {
    param($g)
    $g.DrawLine($whitePen, 52, 44, 52, 84)
    $g.FillEllipse($white, 44, 36, 16, 16)   # top
    $g.FillEllipse($white, 44, 80, 16, 16)   # bottom
    $g.FillEllipse($white, 78, 52, 16, 16)   # branch
    $g.DrawLine($whitePen, 52, 60, 86, 60)
}

# --- github: 暗背景 + 白クラウド+上矢印(公開) ---
Save-Icon "github" "#24292E" {
    param($g)
    # クラウド
    $g.FillEllipse($white, 38, 60, 28, 28)
    $g.FillEllipse($white, 60, 54, 34, 34)
    $g.FillEllipse($white, 80, 64, 22, 22)
    $g.FillRectangle($white, 50, 74, 50, 18)
    # 上矢印(背景色)
    $bg = [System.Drawing.ColorTranslator]::FromHtml("#24292E")
    $pen = New-Object System.Drawing.Pen($bg, 7)
    $pen.StartCap=[System.Drawing.Drawing2D.LineCap]::Round; $pen.EndCap=[System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, 70, 84, 70, 66)
    $g.DrawLine($pen, 70, 66, 62, 74)
    $g.DrawLine($pen, 70, 66, 78, 74)
}

# --- commit: チェック丸 ---
Save-Icon "commit" "#22C55E" {
    param($g)
    $g.FillEllipse($white, 40, 40, 48, 48)
    $bg = [System.Drawing.ColorTranslator]::FromHtml("#22C55E")
    $pen = New-Object System.Drawing.Pen($bg, 8)
    $pen.StartCap=[System.Drawing.Drawing2D.LineCap]::Round; $pen.EndCap=[System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, 52, 64, 61, 74)
    $g.DrawLine($pen, 61, 74, 78, 54)
}

# --- push: 上矢印 ---
Save-Icon "push" "#8B5CF6" {
    param($g)
    $g.DrawLine($whitePen, 64, 92, 64, 50)
    $g.DrawLine($whitePen, 64, 48, 46, 66)
    $g.DrawLine($whitePen, 64, 48, 82, 66)
}

# --- logo: エンジンロゴ(立方体) ---
Save-Icon "logo" "#1E293B" {
    param($g)
    $accent = [System.Drawing.ColorTranslator]::FromHtml("#3B82F6")
    $top = New-Object System.Drawing.Drawing2D.GraphicsPath
    $top.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(64,34)),
        (New-Object System.Drawing.PointF(96,52)),
        (New-Object System.Drawing.PointF(64,70)),
        (New-Object System.Drawing.PointF(32,52))))
    $g.FillPath((New-Object System.Drawing.SolidBrush($accent)), $top)
    $left = New-Object System.Drawing.Drawing2D.GraphicsPath
    $left.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(32,52)),
        (New-Object System.Drawing.PointF(64,70)),
        (New-Object System.Drawing.PointF(64,104)),
        (New-Object System.Drawing.PointF(32,86))))
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,37,99,235))), $left)
    $right = New-Object System.Drawing.Drawing2D.GraphicsPath
    $right.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(96,52)),
        (New-Object System.Drawing.PointF(64,70)),
        (New-Object System.Drawing.PointF(64,104)),
        (New-Object System.Drawing.PointF(96,86))))
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,96,165,250))), $right)
}

# =====================================================================
#  ツールバー / エンティティ種別アイコン（UI視認性向上）
# =====================================================================

function New-WhitePen([float]$w) {
    $p = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $w)
    $p.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $p.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $p.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    return $p
}
function New-BgPen([string]$hex,[float]$w) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    $p = New-Object System.Drawing.Pen($c, $w)
    $p.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $p.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $p.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    return $p
}
function Fill-Poly($g, $brush, $pts) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddPolygon([System.Drawing.PointF[]]$pts)
    $g.FillPath($brush, $path)
}
function PF([float]$x,[float]$y) { New-Object System.Drawing.PointF($x,$y) }

# --- play: 緑背景 + 白の右向き三角 ---
Save-Icon "play" "#22C55E" {
    param($g)
    Fill-Poly $g $white @((PF 52 42),(PF 52 86),(PF 90 64))
}

# --- stop: 赤背景 + 白い角丸スクエア ---
Save-Icon "stop" "#EF4444" {
    param($g)
    $sq = Get-RoundRect 48 48 32 32 6
    $g.FillPath($white, $sq)
}

# --- file: スレート背景 + 白い書類（折り返し角 + 行） ---
Save-Icon "file" "#64748B" {
    param($g)
    $doc = Get-RoundRect 42 32 44 64 6
    $g.FillPath($white, $doc)
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml("#64748B"))
    # 折り返し角
    Fill-Poly $g $bg @((PF 72 32),(PF 86 46),(PF 72 46))
    # 行
    $linePen = New-BgPen "#64748B" 5
    $g.DrawLine($linePen, 52, 58, 78, 58)
    $g.DrawLine($linePen, 52, 70, 78, 70)
    $g.DrawLine($linePen, 52, 82, 70, 82)
}

# --- build: 青背景 + 白い梱包箱（テープ + フラップ）= ゲーム出力 ---
Save-Icon "build" "#3B82F6" {
    param($g)
    $box = Get-RoundRect 40 50 48 40 4
    $g.FillPath($white, $box)
    $bgPen = New-BgPen "#3B82F6" 5
    # テープ（横 + 上半分の縦）
    $g.DrawLine($bgPen, 40, 66, 88, 66)
    $g.DrawLine($bgPen, 64, 50, 64, 66)
    # 上フラップ
    $g.DrawLine($bgPen, 52, 40, 64, 50)
    $g.DrawLine($bgPen, 76, 40, 64, 50)
}

# --- window: ツール窓ドロップダウン用。2x2 のパネル（レイアウト/ウィンドウの象徴） ---
Save-Icon "window" "#6366F1" {
    param($g)
    $g.FillPath($white, (Get-RoundRect 38 38 22 22 4))
    $g.FillPath($white, (Get-RoundRect 68 38 22 22 4))
    $g.FillPath($white, (Get-RoundRect 38 68 22 22 4))
    $g.FillPath($white, (Get-RoundRect 68 68 22 22 4))
}

# --- gizmo_move: 青背景 + 白い4方向矢印 ---
Save-Icon "gizmo_move" "#3B82F6" {
    param($g)
    $p = New-WhitePen 7
    $g.DrawLine($p, 64, 38, 64, 90)
    $g.DrawLine($p, 38, 64, 90, 64)
    # 矢じり
    $a = New-WhitePen 7
    $g.DrawLine($a, 64, 38, 56, 48); $g.DrawLine($a, 64, 38, 72, 48)   # 上
    $g.DrawLine($a, 64, 90, 56, 80); $g.DrawLine($a, 64, 90, 72, 80)   # 下
    $g.DrawLine($a, 38, 64, 48, 56); $g.DrawLine($a, 38, 64, 48, 72)   # 左
    $g.DrawLine($a, 90, 64, 80, 56); $g.DrawLine($a, 90, 64, 80, 72)   # 右
}

# --- gizmo_rotate: 青背景 + 白い回転矢印（円弧 + 矢じり） ---
Save-Icon "gizmo_rotate" "#3B82F6" {
    param($g)
    $p = New-WhitePen 7
    $g.DrawArc($p, 40, 40, 48, 48, 30, 280)
    # 終端（30度 ≈ 右下）に矢じり
    $g.DrawLine($p, 86, 76, 86, 64)
    $g.DrawLine($p, 86, 76, 74, 76)
}

# --- gizmo_scale: 青背景 + 白い対角線 + 両端ハンドル ---
Save-Icon "gizmo_scale" "#3B82F6" {
    param($g)
    $p = New-WhitePen 7
    $g.DrawLine($p, 46, 82, 82, 46)
    $g.FillRectangle($white, 76, 40, 14, 14)   # 大ハンドル
    $g.FillRectangle($white, 42, 78, 10, 10)   # 小ハンドル
}

# --- space_world: スカイ背景 + 白い地球（経線/赤道） ---
Save-Icon "space_world" "#0EA5E9" {
    param($g)
    $p = New-WhitePen 5
    $g.DrawEllipse($p, 40, 40, 48, 48)        # 外周
    $g.DrawEllipse($p, 56, 40, 16, 48)        # 経線（縦楕円）
    $g.DrawLine($p, 41, 64, 87, 64)           # 赤道
}

# --- space_local: スカイ背景 + 白い立方体ワイヤー（オブジェクト空間） ---
Save-Icon "space_local" "#0EA5E9" {
    param($g)
    $p = New-WhitePen 5
    # 上面ひし形
    $g.DrawLine($p, 64, 42, 86, 54); $g.DrawLine($p, 86, 54, 64, 66)
    $g.DrawLine($p, 64, 66, 42, 54); $g.DrawLine($p, 42, 54, 64, 42)
    # 縦エッジ
    $g.DrawLine($p, 42, 54, 42, 78); $g.DrawLine($p, 86, 54, 86, 78); $g.DrawLine($p, 64, 66, 64, 90)
    # 下エッジ
    $g.DrawLine($p, 42, 78, 64, 90); $g.DrawLine($p, 64, 90, 86, 78)
}

# --- ent_mesh: 青背景 + 白いソリッド立方体（3面シェード） ---
Save-Icon "ent_mesh" "#3B82F6" {
    param($g)
    $top  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $lft  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,210,224,245))
    $rgt  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,170,196,235))
    Fill-Poly $g $top @((PF 64 38),(PF 90 53),(PF 64 68),(PF 38 53))
    Fill-Poly $g $lft @((PF 38 53),(PF 64 68),(PF 64 96),(PF 38 81))
    Fill-Poly $g $rgt @((PF 90 53),(PF 64 68),(PF 64 96),(PF 90 81))
}

# --- ent_light: 琥珀背景 + 白い電球（光線つき） ---
Save-Icon "ent_light" "#F59E0B" {
    param($g)
    $g.FillEllipse($white, 50, 40, 28, 28)     # 球
    $g.FillRectangle($white, 58, 64, 12, 12)    # 口金
    $base = New-WhitePen 5
    $g.DrawLine($base, 58, 80, 70, 80)          # 底
    # 光線
    $ray = New-WhitePen 5
    $g.DrawLine($ray, 64, 30, 64, 22)
    $g.DrawLine($ray, 44, 38, 38, 32)
    $g.DrawLine($ray, 84, 38, 90, 32)
    $g.DrawLine($ray, 40, 54, 32, 54)
    $g.DrawLine($ray, 88, 54, 96, 54)
}

# --- ent_camera: 紫背景 + 白いカメラ（本体 + レンズ） ---
Save-Icon "ent_camera" "#8B5CF6" {
    param($g)
    $body = Get-RoundRect 38 50 46 34 5
    $g.FillPath($white, $body)
    # ビューファインダーの出っ張り
    Fill-Poly $g $white @((PF 84 58),(PF 98 50),(PF 98 78),(PF 84 70))
    # レンズ
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml("#8B5CF6"))
    $g.FillEllipse($bg, 50, 58, 20, 20)
    $g.FillEllipse($white, 56, 64, 8, 8)
}

# --- ent_audio: ティール背景 + 白いスピーカー + 音波 ---
Save-Icon "ent_audio" "#14B8A6" {
    param($g)
    # スピーカー本体（四角 + コーン）
    $g.FillRectangle($white, 40, 56, 14, 16)
    Fill-Poly $g $white @((PF 54 56),(PF 72 42),(PF 72 86),(PF 54 72))
    # 音波
    $w = New-WhitePen 5
    $g.DrawArc($w, 74, 50, 16, 28, -55, 110)
    $g.DrawArc($w, 80, 42, 22, 44, -55, 110)
}

# --- ent_script: スレート背景 + 白い </> ---
Save-Icon "ent_script" "#475569" {
    param($g)
    $p = New-WhitePen 7
    $g.DrawLine($p, 56, 50, 44, 64); $g.DrawLine($p, 44, 64, 56, 78)   # <
    $g.DrawLine($p, 72, 50, 84, 64); $g.DrawLine($p, 84, 64, 72, 78)   # >
    $g.DrawLine($p, 70, 46, 58, 82)                                     # /
}

# --- ent_physics: 赤背景 + 白いボール + 軌道アーク（RigidBody） ---
Save-Icon "ent_physics" "#EF4444" {
    param($g)
    $g.FillEllipse($white, 56, 62, 28, 28)     # ボール
    $arc = New-WhitePen 5
    $g.DrawArc($arc, 36, 36, 44, 44, 180, 110)  # 落下軌道
    # 矢じり
    $g.DrawLine($arc, 60, 70, 52, 70)
    $g.DrawLine($arc, 60, 70, 60, 62)
}

# --- ent_collider: 緑背景 + 白い破線のバウンディングボックス ---
Save-Icon "ent_collider" "#22C55E" {
    param($g)
    $p = New-WhitePen 5
    $p.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash
    $g.DrawRectangle($p, 42, 42, 44, 44)
    $solid = New-WhitePen 5
    # 角を実線で強調
    foreach ($c in @(@(42,42),@(86,42),@(42,86),@(86,86))) {
        $g.FillEllipse($white, $c[0]-3, $c[1]-3, 6, 6)
    }
}

# --- ent_empty: スレート背景 + 白い3軸ピボット（空のTransform） ---
Save-Icon "ent_empty" "#64748B" {
    param($g)
    $p = New-WhitePen 6
    $g.DrawLine($p, 64, 64, 64, 40)   # 上
    $g.DrawLine($p, 64, 64, 88, 76)   # 右下
    $g.DrawLine($p, 64, 64, 40, 76)   # 左下
    $g.FillEllipse($white, 59, 59, 10, 10)  # 中心ノード
}

# =====================================================================
#  プロジェクトテンプレート アイコン（ランチャーのテンプレ選択）
# =====================================================================

# --- tmpl_fps: 赤背景 + 白いクロスヘア（一人称の照準）---
Save-Icon "tmpl_fps" "#DC2626" {
    param($g)
    $ring = New-WhitePen 6
    $g.DrawEllipse($ring, 42, 42, 44, 44)        # 外周リング
    $tick = New-WhitePen 6
    $g.DrawLine($tick, 64, 30, 64, 44)           # 上
    $g.DrawLine($tick, 64, 84, 64, 98)           # 下
    $g.DrawLine($tick, 30, 64, 44, 64)           # 左
    $g.DrawLine($tick, 84, 64, 98, 64)           # 右
    $g.FillEllipse($white, 60, 60, 8, 8)         # 中心ドット
}

# --- tmpl_tps: 紫背景 + 白い人物 + 背後カメラ弧（三人称）---
Save-Icon "tmpl_tps" "#7C3AED" {
    param($g)
    $g.FillEllipse($white, 54, 36, 20, 20)       # 頭
    Fill-Poly $g $white @((PF 64 58),(PF 80 92),(PF 48 92))  # 胴(三角)
    # 背後の追従カメラ弧
    $arc = New-WhitePen 5
    $g.DrawArc($arc, 30, 64, 68, 50, 200, 140)
    $g.FillRectangle($white, 86, 78, 12, 10)     # カメラ本体
}

# --- tmpl_2d: エメラルド背景 + 白い太字「2D」（横スクロール）---
Save-Icon "tmpl_2d" "#10B981" {
    param($g)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $fam  = New-Object System.Drawing.FontFamily("Segoe UI")
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddString("2D", $fam, [int][System.Drawing.FontStyle]::Bold, 56,
        (New-Object System.Drawing.PointF(0,0)), [System.Drawing.StringFormat]::GenericDefault)
    $rb = $path.GetBounds()
    $mtx = New-Object System.Drawing.Drawing2D.Matrix
    $mtx.Translate((($S - $rb.Width)/2 - $rb.X), (($S - $rb.Height)/2 - $rb.Y))
    $path.Transform($mtx)
    $g.FillPath($white, $path)
}

# --- tmpl_empty: スレート背景 + 白い破線スクエア + 中央プラス（空のキャンバス）---
Save-Icon "tmpl_empty" "#64748B" {
    param($g)
    $dash = New-WhitePen 5
    $dash.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash
    $g.DrawRectangle($dash, 40, 40, 48, 48)
    $plus = New-WhitePen 6
    $g.DrawLine($plus, 64, 52, 64, 76)
    $g.DrawLine($plus, 52, 64, 76, 64)
}

Write-Host "DONE"
