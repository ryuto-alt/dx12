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

Write-Host "DONE"
