<#
  品質スコアカード（W05-35 / γ4）の撮影。PROGRAM_PLAN §2.3 の「W0 で固定するもの」を、改修前のバイナリでヘッドレスに撮る。

    pwsh tools/scorecard/capture.ps1                       # 全段を %USERPROFILE%\dx12-scorecard\w0\run-<時刻>\ へ
    pwsh tools/scorecard/capture.ps1 -Run r1               # 出力フォルダ名を指定（2 回撮って report.mjs compare する）
    pwsh tools/scorecard/capture.ps1 -Stages splash,ui     # 段を絞る（splash ui golden templates vfx startup）
    node tools/scorecard/report.mjs compare --a <run1> --b <run2>      # 2 回の差が決定論の範囲か
    node tools/scorecard/report.mjs summarize --run <run> --out tests/scorecard/w0.json   # リポジトリの要約を更新

  ・撮るエンジンは既定で %USERPROFILE%\dx12-preoverhaul（tools/nightly.ps1 -Ref pre-overhaul -Out dx12-preoverhaul の出力）。
    nightly.json の ref が pre-overhaul でなければ拒否する（-AllowNonPreOverhaul で試走だけ可。要約に preOverhaul=false が残る）。
  ・窓を出さない: エンジンは --headless / --background（tools/engine_instance.ps1 経由）、スプラッシュは --splash-preview（窓なし）。
    ビルド出力のフォルダは握らない（exe は常にインスタンス専用フォルダへコピーして使う）。
  ・改修後のバイナリを撮るときは -NoDred（エンジンへ --no-dred を渡す）。
  ・LS-A〜F は W1 の NB3 の後で段を足す。MCP-14 は W05-37、性能は W05-39（このスクリプトでは撮らない）。
  ・ANTHROPIC_API_KEY があれば起動を拒む。
#>
param(
    [string]$EngineDir = '',
    [string]$Out = '',
    [string]$Run = '',
    [string[]]$Stages = @('splash', 'ui', 'golden', 'templates', 'vfx', 'startup'),
    [int]$Port = 8854,
    [int]$StartupRuns = 6,
    [switch]$NoDred,
    [switch]$AllowNonPreOverhaul
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

foreach ($scope in 'Process', 'User', 'Machine') {
    if ([Environment]::GetEnvironmentVariable('ANTHROPIC_API_KEY', $scope)) {
        Write-Host "[scorecard] ANTHROPIC_API_KEY が $scope に設定されている。API 課金を避けるため起動を拒否する。"; exit 3
    }
}
$valid = 'splash', 'ui', 'golden', 'templates', 'vfx', 'startup'
$Stages = @($Stages | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($s in $Stages) { if ($valid -notcontains $s) { Write-Host "[scorecard] 不明な段: $s（$($valid -join ' ')）"; exit 2 } }

$home2 = $env:USERPROFILE
if (-not $EngineDir) { $EngineDir = Join-Path $home2 'dx12-preoverhaul' }
if (-not $Out) { $Out = Join-Path $home2 'dx12-scorecard\w0' }
if (-not $Run) { $Run = 'run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
if ($Run -notmatch '^[A-Za-z0-9._-]+$') { Write-Host '[scorecard] -Run は英数字と . _ - だけ'; exit 2 }
$EngineDir = [IO.Path]::GetFullPath($EngineDir)
$runDir = Join-Path ([IO.Path]::GetFullPath($Out)) $Run
if (-not (Test-Path (Join-Path $EngineDir 'DX12Engine.exe'))) { Write-Host "[scorecard] エンジンが無い: $EngineDir（tools/nightly.ps1 -Ref pre-overhaul -Out dx12-preoverhaul で用意する）"; exit 4 }

# --- 改修前のバイナリか
$nj = $null
$njPath = Join-Path $EngineDir 'nightly.json'
if (Test-Path $njPath) { $nj = Get-Content $njPath -Raw | ConvertFrom-Json }
$preOverhaul = ($null -ne $nj -and $nj.ref -eq 'pre-overhaul')
if (-not $preOverhaul -and -not $AllowNonPreOverhaul) {
    Write-Host "[scorecard] $EngineDir は改修前のビルドではない（nightly.json の ref=$($nj.ref)）。W0 は dx12-preoverhaul で撮る。試走なら -AllowNonPreOverhaul。"; exit 5
}

# --- exe のコピー（ビルド出力を握らない / 窓の無いツール起動用）
$tool = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-instances\scorecard-w0-tool'
New-Item -ItemType Directory -Force $tool | Out-Null
robocopy $EngineDir $tool DX12Engine.exe *.dll /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($d in 'D3D12', 'shaders') { if (Test-Path (Join-Path $EngineDir $d)) { robocopy (Join-Path $EngineDir $d) (Join-Path $tool $d) /MIR /NFL /NDL /NJH /NJS /NP | Out-Null } }
$assetsSrc = if (Test-Path (Join-Path $EngineDir 'assets')) { Join-Path $EngineDir 'assets' } else { Join-Path $root 'assets' }
robocopy $assetsSrc (Join-Path $tool 'assets') /MIR /XF 'splash_d.*' /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host '[scorecard] コピーに失敗'; exit 6 }

New-Item -ItemType Directory -Force $runDir | Out-Null
try { [Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal' } catch {}

# --- メタ情報（絶対パスは書かない）
$gpu = @(); try { $gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { @{ name = $_.Name; driver = $_.DriverVersion } }) } catch {}
$exeHash = (Get-FileHash (Join-Path $EngineDir 'DX12Engine.exe') -Algorithm SHA256).Hash.ToLower()
$captureCommit = (git -C $root rev-parse --short HEAD 2>$null)
$meta = [ordered]@{
    preOverhaul = $preOverhaul; engineRef = $nj.ref; engineCommit = $nj.commit; engineVersion = $nj.engineVersion
    exeSha256 = $exeHash; noDred = [bool]$NoDred; captureScriptsCommit = $captureCommit
    gpu = $gpu; os = [Environment]::OSVersion.VersionString; capturedAt = (Get-Date).ToString('s'); stages = $Stages
}
($meta | ConvertTo-Json -Depth 5) | Set-Content (Join-Path $runDir 'meta.json') -Encoding utf8

# --- 段ごとに撮る
$node = (Get-Command node).Source
$failed = @()
foreach ($s in $Stages) {
    Write-Host "[scorecard] 段: $s"
    $a = @((Join-Path $PSScriptRoot 'capture_engine.mjs'), '--stage', $s, '--engine-dir', $EngineDir, '--run-dir', $runDir,
        '--tool-dir', $tool, '--port', "$Port", '--startup-runs', "$StartupRuns")
    if ($NoDred) { $a += '--no-dred' }
    & $node @a
    if ($LASTEXITCODE -ne 0) { $failed += $s }
}
& $node (Join-Path $PSScriptRoot 'report.mjs') summarize --run $runDir
Write-Host "[scorecard] 出力: $runDir  失敗した段: $(if ($failed) { $failed -join ',' } else { 'なし' })"
exit $(if ($failed) { 1 } else { 0 })
