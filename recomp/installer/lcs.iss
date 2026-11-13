; Inno Setup script: LCS-PC-Setup.exe for the GTA: Liberty City Stories PC port.
; Compile:  iscc recomp\installer\lcs.iss   (output: build\installer\LCS-PC-Setup.exe)
;
; The installer contains no game code or data. On the player's PC it
;   1. copies the game data from their own Android copy (lcs-data.exe: ADB, or APK files),
;   2. downloads a free toolchain (llvm-mingw, embeddable Python, capstone, pyelftools; checked by SHA-256),
;   3. recompiles their libGame.so into lcs.exe (recomp/build.py), then deletes the toolchain.
; Saves and settings live in Documents\Rockstar Games\GTA Liberty City Stories.

#define AppName "GTA Liberty City Stories PC"
#define AppVersion "0.1.0"
#define Root "..\.."
#define Sdl "..\..\.tools\sdl3\SDL3-3.4.18"

#define LlvmUrl "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip"
#define LlvmSha "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd"
#define PyUrl "https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip"
#define PySha "009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b"
#define CsUrl "https://files.pythonhosted.org/packages/da/72/ff7894c2fb5716d9a3ce9c27ba34b29d991a11d8442d2ef0fcdc5564ba7e/capstone-5.0.6-py3-none-win_amd64.whl"
#define CsSha "761c3deae00b22ac697081cdae1383bb90659dd0d79387a09cf5bdbb22b17064"
#define ElfUrl "https://files.pythonhosted.org/packages/af/43/700932c4f0638c3421177144a2e86448c0d75dbaee2c7936bda3f9fd0878/pyelftools-0.32-py3-none-any.whl"
#define ElfSha "013df952a006db5e138b1edf6d8a68ecc50630adbd0d83a2d41e7f846163d738"

[Setup]
AppId={{5321DD84-2DEF-431E-89E9-100B4F4F2F93}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Root}\build\installer
OutputBaseFilename=LCS-PC-Setup
UninstallDisplayIcon={app}\lcs.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; game data (~2.5 GB) plus the temporary toolchain and generated sources (~1.5 GB)
ExtraDiskSpaceRequired=4200000000

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#Root}\build\installer\lcs-data.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Sdl}\lib\x64\SDL3.dll"; DestDir: "{app}"; Flags: ignoreversion
; recompiler and runtime sources, used on this PC
Source: "{#Root}\recomp\build.py"; DestDir: "{app}\src\recomp"; Flags: ignoreversion
Source: "{#Root}\recomp\lift.py"; DestDir: "{app}\src\recomp"; Flags: ignoreversion
Source: "{#Root}\recomp\a64.py"; DestDir: "{app}\src\recomp"; Flags: ignoreversion
Source: "{#Root}\recomp\gen_gl.py"; DestDir: "{app}\src\recomp"; Flags: ignoreversion
Source: "{#Root}\recomp\tools\make_icon.py"; DestDir: "{app}\src\recomp\tools"; Flags: ignoreversion
Source: "{#Root}\recomp\rt\*.c"; DestDir: "{app}\src\recomp\rt"; Flags: ignoreversion
Source: "{#Root}\recomp\rt\*.h"; DestDir: "{app}\src\recomp\rt"; Flags: ignoreversion
Source: "{#Sdl}\include\*"; DestDir: "{app}\src\.tools\sdl3\SDL3-3.4.18\include"; Flags: ignoreversion recursesubdirs
Source: "{#Sdl}\lib\x64\SDL3.lib"; DestDir: "{app}\src\.tools\sdl3\SDL3-3.4.18\lib\x64"; Flags: ignoreversion
Source: "{#Sdl}\lib\x64\SDL3.dll"; DestDir: "{app}\src\.tools\sdl3\SDL3-3.4.18\lib\x64"; Flags: ignoreversion
Source: "{#Sdl}\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "SDL3-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\lcs.exe"
Name: "{group}\Save games and settings"; Filename: "{userdocs}\Rockstar Games\GTA Liberty City Stories"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\lcs.exe"; Tasks: desktopicon

[Dirs]
Name: "{userdocs}\Rockstar Games\GTA Liberty City Stories"; Flags: uninsneveruninstall

[Run]
Filename: "{app}\lcs.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Check: GameBuilt

[UninstallDelete]
; game data, the built game, ADB tools and build leftovers; saves in Documents are kept
Type: filesandordirs; Name: "{app}\data"
Type: filesandordirs; Name: "{app}\platform-tools"
Type: filesandordirs; Name: "{app}\src"
Type: files; Name: "{app}\lcs.exe"
Type: files; Name: "{app}\build.log"

[Code]
var
  SourcePage: TInputOptionWizardPage;
  ApkPage: TInputDirWizardPage;
  ObbPage: TInputFileWizardPage;
  DownloadPage: TDownloadWizardPage;

procedure InitializeWizard;
begin
  SourcePage := CreateInputOptionPage(wpSelectDir, 'Game data',
    'Where should the game data come from?',
    'The PC port is built from your own copy of GTA: Liberty City Stories for Android (version 2.4, 64-bit).'#13#10 +
    'Its data (about 2.5 GB) is copied, and its program code is recompiled for Windows on this PC.', True, False);
  SourcePage.Add('From my Android phone over USB (USB debugging / ADB)');
  SourcePage.Add('From APK files already on this PC (base.apk, split_config.arm64_v8a.apk, split_data_main.apk)');
  SourcePage.SelectedValueIndex := 0;

  ApkPage := CreateInputDirPage(SourcePage.ID, 'APK files', 'Folder that contains the APK files',
    'Select the folder with base.apk, split_config.arm64_v8a.apk and split_data_main.apk.', False, '');
  ApkPage.Add('');

  ObbPage := CreateInputFilePage(ApkPage.ID, 'Radio (optional)', 'Old expansion file for the radio stations',
    'The current Android release has no radio music. If you have the older main.*.com.rockstargames.gtalcs.obb, ' +
    'select it to add the radio stations. Leave empty to skip.');
  ObbPage.Add('OBB file:', 'OBB files|*.obb|All files|*.*', '.obb');

  DownloadPage := CreateDownloadPage('Downloading the build tools',
    'A free compiler and Python are downloaded to recompile the game on this PC (about 200 MB). They are removed afterwards.', nil);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = ApkPage.ID) or (PageID = ObbPage.ID)) and (SourcePage.SelectedValueIndex = 0);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = ApkPage.ID) and not FileExists(AddBackslash(ApkPage.Values[0]) + 'base.apk') then begin
    MsgBox('base.apk was not found in that folder.', mbError, MB_OK);
    Result := False;
  end;
  if CurPageID = wpReady then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#LlvmUrl}', 'llvm-mingw.zip', '{#LlvmSha}');
    DownloadPage.Add('{#PyUrl}', 'python-embed.zip', '{#PySha}');
    DownloadPage.Add('{#CsUrl}', 'capstone.whl', '{#CsSha}');
    DownloadPage.Add('{#ElfUrl}', 'pyelftools.whl', '{#ElfSha}');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if DownloadPage.AbortedByUser then
          Log('Download aborted by user.')
        else
          SuppressibleMsgBox(AddPeriod(GetExceptionMessage), mbCriticalError, MB_OK, IDOK);
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

function GameBuilt: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\lcs.exe'));
end;

function Tar(Archive, Dest: String): Boolean;
var
  Code: Integer;
begin
  ForceDirectories(Dest);
  Result := Exec(ExpandConstant('{sys}\tar.exe'), '-xf "' + Archive + '" -C "' + Dest + '"', '', SW_HIDE,
                 ewWaitUntilTerminated, Code) and (Code = 0);
end;

function ImportData: Boolean;
var
  Params: String;
  Code: Integer;
begin
  Params := '--dest "' + ExpandConstant('{app}\data') + '"';
  { setup.exe /APKS="folder" (unattended installs and tests) }
  if ExpandConstant('{param:APKS|}') <> '' then
    Params := Params + ' --apks "' + ExpandConstant('{param:APKS|}') + '"'
  else if SourcePage.SelectedValueIndex = 1 then begin
    Params := Params + ' --apks "' + ApkPage.Values[0] + '"';
    if ObbPage.Values[0] <> '' then
      Params := Params + ' --obb "' + ObbPage.Values[0] + '"';
  end;
  Result := Exec(ExpandConstant('{app}\lcs-data.exe'), Params, ExpandConstant('{app}'), SW_SHOW, ewWaitUntilTerminated, Code)
            and (Code = 0);
end;

function PrepareToolchain: Boolean;
var
  Tools, Tmp: String;
begin
  Tools := ExpandConstant('{app}\src\toolchain');
  Tmp := ExpandConstant('{tmp}');
  if DirExists(Tools + '\llvm\bin') and FileExists(Tools + '\python\python.exe') then begin
    Result := True;
    exit;
  end;
  DelTree(Tools, True, True, True);
  Result := Tar(Tmp + '\llvm-mingw.zip', Tools) and RenameFile(Tools + '\llvm-mingw-20261020-ucrt-x86_64', Tools + '\llvm')
        and Tar(Tmp + '\python-embed.zip', Tools + '\python')
        and Tar(Tmp + '\capstone.whl', Tools + '\python\site')
        and Tar(Tmp + '\pyelftools.whl', Tools + '\python\site')
        and SaveStringToFile(Tools + '\python\python311._pth', 'python311.zip'#13#10'.'#13#10'site'#13#10, False);
end;

function BuildGame: Boolean;
var
  App, Cmd: String;
  Code: Integer;
begin
  App := ExpandConstant('{app}');
  Cmd := '"' + App + '\src\toolchain\python\python.exe" "' + App + '\src\recomp\build.py"' +
         ' --libs "' + App + '\data\lib" --out "' + App + '\src\gen" --exe "' + App + '\lcs.exe"' +
         ' --llvm "' + App + '\src\toolchain\llvm\bin" --icon-res "' + App + '\data\res"';
  SaveStringToFile(App + '\src\build.cmd',
    '@echo off'#13#10 +
    'title Recompiling GTA: Liberty City Stories for Windows'#13#10 +
    'echo Recompiling the game from your libGame.so. This takes a few minutes.'#13#10 +
    Cmd + ' > "' + App + '\build.log" 2>&1'#13#10 +
    'if errorlevel 1 (type "' + App + '\build.log" & echo. & echo Build failed. The log is in build.log. & pause & exit /b 1)'#13#10, False);
  Result := Exec(ExpandConstant('{cmd}'), '/c "' + App + '\src\build.cmd"', App, SW_SHOW, ewWaitUntilTerminated, Code)
            and (Code = 0) and FileExists(App + '\lcs.exe');
  if Result then begin
    DelTree(App + '\src\gen', True, True, True);
    DelTree(App + '\src\toolchain', True, True, True);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    WizardForm.StatusLabel.Caption := 'Copying game data (see the console window)...';
    while not ImportData do begin
      if MsgBox('The game data was not imported. Try again?', mbConfirmation, MB_YESNO) = IDNO then
        exit;
    end;
    WizardForm.StatusLabel.Caption := 'Unpacking the build tools...';
    if not PrepareToolchain then begin
      MsgBox('The build tools could not be unpacked.', mbError, MB_OK);
      exit;
    end;
    WizardForm.StatusLabel.Caption := 'Recompiling the game for Windows (a few minutes)...';
    while not BuildGame do begin
      if MsgBox('Recompiling the game failed (see build.log in the install folder). Try again?',
                mbConfirmation, MB_YESNO) = IDNO then
        exit;
    end;
  end;
end;
