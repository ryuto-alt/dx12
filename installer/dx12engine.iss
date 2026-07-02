; DX12 Engine 個人用インストーラー（Inno Setup）
; - 管理者不要（%LOCALAPPDATA%\Programs\DX12Engine へインストール）
; - 既存の GitHub リリース自動更新(Updater)はそのまま動く（書き込み可能フォルダのため）
; ビルド: ISCC.exe installer\dx12engine.iss  （/DMyAppVersion=0.5.1 で版を上書き可）

#ifndef MyAppVersion
  #define MyAppVersion "0.5.2"
#endif
#define MyAppName "DX12 Engine"
#define MyAppPublisher "ryuto-alt"
#define MyAppURL "https://github.com/ryuto-alt/dx12"
#define MyAppExeName "DX12Engine.exe"
#define RepoRoot ".."
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
DefaultGroupName=DX12 Engine
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
Source: "{#SrcDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
; MCP(AI ブリッジ)サーバを同梱。エンジンは exe の隣の tools\mcp-server\index.ts を実行時に探すので、
; ここに入れておけば配布先でも「MCP / AI Bridge」窓のコマンドがそのまま繋がる（node_modules も含めて同梱＝npm install 不要）。
; ※ node_modules は .gitignore 済みなので、ビルド前に tools\mcp-server で `npm install` を実行しておくこと。
;    installer\build.ps1 を使えば自動で行う。
Source: "{#RepoRoot}\tools\mcp-server\*"; DestDir: "{app}\tools\mcp-server"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: ".gitignore,*.log,*.tsbuildinfo"
; VSCode 補完用の Lua API 型定義（導入手順は同フォルダの README.md）
Source: "{#RepoRoot}\tools\lua-defs\*"; DestDir: "{app}\tools\lua-defs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
