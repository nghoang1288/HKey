; HKey for Windows installer (Inno Setup 6). Per-user, no admin prompt, so the app can update itself.
;   iscc /DVersion=1.0.25 /DArch=x64 /DSrc=..\build\App HKey.iss
#ifndef Version
  #define Version "1.0.25"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef Src
  #define Src "..\build\App"
#endif

[Setup]
AppId={{7E2B5C1A-9D3F-4C7B-8E61-HKEY00000001}
AppName=HKey
AppVersion={#Version}
AppVerName=HKey {#Version}
AppPublisher=nghoang1288
AppPublisherURL=https://github.com/nghoang1288/HKey
AppSupportURL=https://github.com/nghoang1288/HKey/issues
AppUpdatesURL=https://github.com/nghoang1288/HKey/releases
VersionInfoVersion={#Version}
DefaultDirName={localappdata}\Programs\HKey
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=.\out
OutputBaseFilename=HKey-{#Version}-{#Arch}-Setup
SetupIconFile=..\App\HKey.ico
UninstallDisplayIcon={app}\HKey.exe
UninstallDisplayName=HKey
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=HKey.exe
RestartApplications=no
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
SetupWindowTitle=Cài đặt HKey
SetupAppTitle=HKey
SetupAppTitleMerge=HKey
ButtonNext=Tiếp tục >
ButtonInstall=Cài đặt
ButtonCancel=Huỷ
FinishedHeadingLabel=Cài đặt HKey hoàn tất
FinishedLabel=HKey đã được cài đặt trên máy bạn.
ClickFinish=Bấm Hoàn tất để đóng trình cài đặt.
ExitSetupTitle=Thoát cài đặt
ExitSetupMessage=HKey chưa được cài đặt xong. Bạn có chắc muốn thoát?

[Files]
Source: "{#Src}\HKey.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\HKey-Portable\macro.txt"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist

[Icons]
Name: "{userprograms}\HKey"; Filename: "{app}\HKey.exe"
Name: "{userdesktop}\HKey"; Filename: "{app}\HKey.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Tạo biểu tượng trên màn hình nền (Desktop)"; Flags: unchecked

[Run]
Filename: "{app}\HKey.exe"; Description: "Mở HKey"; Flags: nowait postinstall skipifsilent
