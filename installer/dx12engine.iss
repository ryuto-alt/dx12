; Uno Engine 個人用インストーラー（Inno Setup）
; - 管理者不要（%LOCALAPPDATA%\Programs\DX12Engine へインストール）
; - 既存の GitHub リリース自動更新(Updater)はそのまま動く（書き込み可能フォルダのため）
; ビルド: pwsh installer\build.ps1（cmake\agility.lock.json の版を /DAgilityVersion で渡す。ISCC を直接呼ぶときは /DAgilityVersion=<SDK 版> も要る。/DMyAppVersion=0.5.1 で版を上書き可）

#ifndef MyAppVersion
  #define MyAppVersion "0.5.2"
#endif
#define MyAppName "Uno Engine"
#define MyAppPublisher "ryuto-alt"
#define MyAppURL "https://github.com/ryuto-alt/dx12"
#define MyAppExeName "DX12Engine.exe"
#define RepoRoot ".."
; Agility SDK の版。installer\build.ps1 が cmake\agility.lock.json から /DAgilityVersion=... で渡す（版の数字はここに書かない）。
#ifndef AgilityVersion
  #error AgilityVersion が未定義です。installer\build.ps1 経由で生成してください（cmake\agility.lock.json の version）。
#endif
#define SrcDir RepoRoot + "\build\release"

[Setup]
; 同一アプリの再インストール/アップグレード検出用の固定 ID（変更しないこと）
AppId={{7C9E6A2B-3F1D-4B8A-9E2C-5D6F1A2B3C4D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\DX12Engine
DefaultGroupName=Uno Engine
DisableProgramGroupPage=yes
; 管理者不要の個人用インストール（自己更新が UAC 無しで効くようにするため）
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile={#RepoRoot}\resources\app.ico
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir=output
OutputBaseFilename=DX12Engine-Setup-v{#MyAppVersion}

[Languages]
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SrcDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\GameRuntime.exe"; DestDir: "{app}"; Flags: ignoreversion
; gh.exe(GitHub CLI、同梱): エディタの GitHub 連携(リポジトリ作成/push/ログイン)に必要。
; CMake configure 時に third_party/gh-cli/ へ自動取得され、ビルド後に exe の隣へコピーされる。
Source: "{#SrcDir}\gh.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; nvngx_dlss.dll もこのワイルドカードで入る（DLSS 有効ビルドのとき）。-Dlss off のときだけ DllExcludes で外す。
#ifdef DllExcludes
Source: "{#SrcDir}\*.dll"; DestDir: "{app}"; Excludes: "{#DllExcludes}"; Flags: ignoreversion
#else
Source: "{#SrcDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
#endif
#ifdef DlssLicense
; NVIDIA DLSS SDK のライセンス文（nvngx_dlss.dll を配るときは必ずセット）。ビルドが SDK の LICENSE.txt を exe の隣の licenses へコピーした物を使う（UNO_DLSS_SDK_DIR を替えても DLL と一致する）。
Source: "{#SrcDir}\licenses\NVIDIA_DLSS_LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
#endif
; D3D12 Agility SDK のランタイム。D3D12\* のワイルドカードは使わない（ビルドツリーには d3d12SDKLayers.dll もあり、配布しないため）。
Source: "{#SrcDir}\D3D12\D3D12Core.dll"; DestDir: "{app}\D3D12"; Flags: ignoreversion
; Agility の再頒布ライセンス文（third_party\agility は configure が取得する物）
Source: "{#RepoRoot}\third_party\agility\{#AgilityVersion}\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "Microsoft.Direct3D.D3D12-LICENSE.txt"; Flags: ignoreversion
; UI 書体（IBM Plex / Noto Sans JP / PlemolJP）の SIL OFL 1.1 ライセンス文。書体を配る以上は必ず同梱する（tools/subset_fonts.py --check が揃っているか検査）。
Source: "{#RepoRoot}\third_party\licenses\IBM-Plex-OFL.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\licenses\Noto-OFL.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\licenses\PlemolJP-OFL.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; UI 書体のサブセット（IBM Plex / PlemolJP / Noto Sans JP。W1-A14）。ImGuiManager が exe 隣の assets\editor\fonts\ui\ から読む。
Source: "{#RepoRoot}\third_party\fonts\ibm-plex\subset\*.ttf"; DestDir: "{app}\assets\editor\fonts\ui"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\fonts\plemoljp\subset\*.ttf"; DestDir: "{app}\assets\editor\fonts\ui"; Flags: ignoreversion
Source: "{#RepoRoot}\third_party\fonts\noto-sans-jp\subset\*.ttf"; DestDir: "{app}\assets\editor\fonts\ui"; Flags: ignoreversion
Source: "{#SrcDir}\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs createallsubdirs
; シェーダーソース(.hlsl/.hlsli)。配布エディタでのシェーダーホットリロード/エンジンシェーダー編集用
; (PathResolver::ShaderSourceDirW() が配布時に exe 隣の shaders-src/ を見る)。
Source: "{#RepoRoot}\shaders\*"; DestDir: "{app}\shaders-src"; Flags: ignoreversion recursesubdirs createallsubdirs
; 起動音 splash_d.*（出どころ/ライセンス未確認）は配布しない。開発機に変換済み WAV があっても入れない。
Source: "{#RepoRoot}\assets\*"; DestDir: "{app}\assets"; Excludes: "splash_d.*,editor\sounds\calibrate\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; MCP(AI ブリッジ)サーバは同梱しない。エディタの「MCP / AI Bridge」窓のボタン 1 回（または DX12Engine.exe --install-mcp）で
; https://github.com/ryuto-alt/dx12-mcp から取得し、Node.js も無ければ自動で入れて Codex / Claude Code などへ登録する。
; VSCode 補完用の Lua API 型定義（導入手順は同フォルダの README.md）
Source: "{#RepoRoot}\tools\lua-defs\*"; DestDir: "{app}\tools\lua-defs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "lab-ssh\Start-UnoLabTunnel.ps1"; DestDir: "{app}\tools\lab-ssh"; Flags: ignoreversion
; ArduinoLab（Arduino / ESP32 の実験キット。初回起動時に Documents\UnoProjects へ置かれる）とファームウェア
Source: "{#RepoRoot}\hardware\lab\ArduinoLab\*"; DestDir: "{app}\samples\ArduinoLab"; Excludes: ".dx12,.autosave"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\hardware\lab\GroveLab\*"; DestDir: "{app}\samples\GroveLab"; Excludes: ".dx12,.autosave"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\hardware\firmware\UnoLink\*"; DestDir: "{app}\hardware\firmware\UnoLink"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\hardware\firmware\UnoLinkGeneric\*"; DestDir: "{app}\hardware\firmware\UnoLinkGeneric"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\hardware\firmware\README.md"; DestDir: "{app}\hardware\firmware"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
