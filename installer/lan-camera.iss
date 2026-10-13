; Inno Setup script for the LAN Phone Camera virtual camera driver.
; Built by vcam\build.bat:  ISCC.exe /DAppVersion=<version> /DBuildDir=<vcam\build> /DOutDir=<output folder> lan-camera.iss
; Output: lan-camera-setupv<version>.exe  (one self-contained installer)

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build"
#endif
#ifndef OutDir
  #define OutDir "..\build\installer"
#endif

[Setup]
AppId={{7E4C2A15-3B8D-4F6A-9C21-5D0E8A1B6F73}
AppName=LAN Phone Camera
AppVersion={#AppVersion}
AppVerName=LAN Phone Camera {#AppVersion}
AppPublisher=LAN Camera Streaming
VersionInfoVersion={#AppVersion}
VersionInfoProductName=LAN Phone Camera
VersionInfoDescription=LAN Phone Camera virtual camera driver setup
DefaultDirName={autopf}\LanCamera
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutDir}
OutputBaseFilename=lan-camera-setupv{#AppVersion}
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=LAN Phone Camera
UninstallDisplayIcon={app}\lancam-setup.exe
CloseApplications=no
RestartIfNeededByRun=no
#if FileExists(SourcePath + "..\..\assets\icon.ico")
SetupIconFile=..\..\assets\icon.ico
#endif

[Files]
Source: "{#BuildDir}\lancam-setup.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#BuildDir}\LanCamFilter.dll"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#BuildDir}\LanCamMfSource.dll"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete

[UninstallRun]
Filename: "{app}\lancam-setup.exe"; Parameters: "--uninstall --silent"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterLanCamera"

[UninstallDelete]
Type: files; Name: "{app}\*.old*"
Type: filesandordirs; Name: "{commonappdata}\LanCamera"

[Code]
// Browsers, Electron apps (Chrome, Claude, VS Code ...) and the Windows Frame Server keep the camera DLLs loaded.
// Windows lets a loaded DLL be renamed but not overwritten, so move any existing copy aside before Inno copies the
// new one; the old copy is deleted at the next reboot.
procedure MoveAsideInUse(const FileName: string);
var
  Aside: string;
  Index: Integer;
begin
  if not FileExists(FileName) then
    Exit;
  if DeleteFile(FileName) then
    Exit;
  for Index := 0 to 9 do begin
    Aside := FileName + '.old' + IntToStr(Index);
    if (not FileExists(Aside)) or DeleteFile(Aside) then begin
      if RenameFile(FileName, Aside) then begin
        RestartReplace(Aside, '');
        Exit;
      end;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  AppDir: string;
begin
  AppDir := ExpandConstant('{app}');

  if CurStep = ssInstall then begin
    Exec(ExpandConstant('{sys}\net.exe'), 'stop FrameServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    MoveAsideInUse(AppDir + '\LanCamFilter.dll');
    MoveAsideInUse(AppDir + '\LanCamMfSource.dll');
    MoveAsideInUse(AppDir + '\lancam-setup.exe');
  end;

  if CurStep = ssPostInstall then begin
    WizardForm.StatusLabel.Caption := 'Registering the LAN Phone Camera...';
    if not Exec(AppDir + '\lancam-setup.exe', '--install --silent', AppDir, SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      ResultCode := -1;
    if (ResultCode <> 0) and (not WizardSilent) then
      MsgBox('The files were installed, but part of the camera registration failed.' + #13#10 + #13#10 +
             'The DirectShow camera or the Windows camera may be missing (the Windows camera needs Windows 11).' + #13#10 +
             'Details: ' + ExpandConstant('{commonappdata}') + '\LanCamera\setup.log',
             mbError, MB_OK);
  end;
end;
