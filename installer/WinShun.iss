; Win顺 installer (Inno Setup 6.5 or later: the Windows 11 wizard style with
; dark mode). Built by scripts/release.ps1, which passes:
;   /DAppVersion=0.2.1  /DSourceDir=<dist\WinShun>  /DOutputDir=<dist>
; Upgrading is installing again: the same AppId replaces the old version in
; place, and the user's settings and index (in %APPDATA% / %LOCALAPPDATA%)
; are not touched.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\WinShun"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "Win顺"
#define AppExe "WinShun.exe"
#define Repository "https://github.com/LingCore/WinShun"

[Setup]
; Never change the AppId: it is how a new version finds the old one.
AppId={{C582F95C-EAF2-4AF2-961D-E35C4506652A}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=LingCore
AppPublisherURL={#Repository}
AppSupportURL={#Repository}/issues
AppUpdatesURL={#Repository}/releases
AppCopyright=Copyright © 2026 LingCore
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName} · WinShun
VersionInfoDescription={#AppName} Setup
VersionInfoCompany=LingCore

; Installed for all users under Program Files: the index service (which
; reads the NTFS master file tables for WinShun, GitHub #21) is registered
; once, by this installer; WinShun itself runs with the user's normal rights.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DefaultDirName={autopf}\WinShun
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableReadyPage=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
; It is closed in PrepareToInstall, the way it saves its index.
CloseApplications=no
RestartApplications=no

WizardStyle=modern dynamic windows11
; Win顺's own pictures, light and dark (tools/make_installer_images.py).
WizardImageFile=wizard-light.png
WizardImageFileDynamicDark=wizard-dark.png
WizardSmallImageFile=wizard-small.png
WizardSmallImageFileDynamicDark=wizard-small.png
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}

ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage

OutputDir={#OutputDir}
OutputBaseFilename=WinShun-{#AppVersion}-x64-setup
Compression=lzma2/ultra64
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[CustomMessages]
english.RemoveData=Also delete WinShun's settings, file index and search history?%n%nKeep them if you are going to install WinShun again.
chinesesimplified.RemoveData=要同时删除 Win顺 的设置、文件索引和搜索记录吗？%n%n如果还会再装 Win顺，可以保留它们。
; The Start menu shortcut users pin to the taskbar (WinShunSearch.exe).
english.SearchShortcut=WinShun Search
chinesesimplified.SearchShortcut=Win顺 搜索

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything build.ps1 -Deploy put in dist\WinShun, the index service
; WinShunService.exe (src/service) among them: it is stopped in
; PrepareToInstall before the files are replaced, and registered again in [Run].
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
; The taskbar button. Its AppUserModelID, the one WinShunSearch.exe sets for
; itself, lets it ask Windows to be pinned (settings -> 打开 Win顺 -> 固定).
Name: "{autoprograms}\{cm:SearchShortcut}"; Filename: "{app}\WinShunSearch.exe"; AppUserModelID: "LingCore.WinShun.Search"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; The index service (GitHub #21): registered as the service "WinShun"
; (LocalSystem, automatic start) and started, by the installer's own
; (elevated) rights; installed again, it is pointed at the new files.
Filename: "{app}\WinShunService.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated runascurrentuser
; The rest as the user who started Setup, with normal rights (runasoriginaluser):
; WinShun runs so, and its autostart task and settings are that user's, not
; the administrator's who answered UAC for a standard user (GitHub #21).
; An autostart set up earlier (a portable copy, the version before) now
; starts this copy; PrepareToInstall removed this copy's old task, which
; may have been that administrator's (--had-autostart: write one anyway).
Filename: "{app}\{#AppExe}"; Parameters: "--take-autostart {code:TakeAutostartArgs}"; Flags: runhidden waituntilterminated runasoriginaluser
; Started at once, in the tray, while the last page is up: the first start of
; new files is slow (the antivirus reads each one: seconds, tens of them at
; times), and nothing would show for that long after Finish. Finish then has the running
; copy put up its search box, at once (the second start hands it "show"), or,
; unticked, has it quit (CurStepChanged). Not runhidden: a hidden start would
; hide the first window it shows later on.
Filename: "{app}\{#AppExe}"; Parameters: "--background"; Flags: nowait runasoriginaluser
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
const
  MessageWindowClass = 'WinShun.MessageWindow';
  WM_CLOSE = $0010;

function WinShunRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /NH /FI "IMAGENAME eq {#AppExe}" | find /I "{#AppExe}"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// Asks a running WinShun to quit, the way it saves its index first: with
// --quit through the installed copy, or WM_CLOSE to its message window (a
// portable copy elsewhere; WinShun 0.2.1 and later). One still running after
// a while is ended.
// First the MCP relays (WinShunMcp.exe): an AI client (Claude, Cursor, VS
// Code) keeps one running for as long as it is open, with Qt6Core.dll and
// msvcp140.dll from the install folder loaded, so those could not be
// replaced (DeleteFile, code 5). Ended before WinShun, too: one would start
// WinShun again through its autostart task when a tool is called.
procedure CloseWinShun(Exe: String);
var
  ResultCode, Waited: Integer;
begin
  if Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM WinShunMcp.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    Sleep(500); // for the files to be let go
  if not WinShunRunning() then
    exit;
  if FileExists(Exe) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
  else
    PostMessage(FindWindowByClassName(MessageWindowClass), WM_CLOSE, 0, 0);
  Waited := 0;
  while WinShunRunning() and (Waited < 15000) do
  begin
    Sleep(500);
    Waited := Waited + 500;
  end;
  if WinShunRunning() then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExe}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);
  end;
end;

// MoveFileExW: with no new name and MOVEFILE_DELAY_UNTIL_REBOOT, the file
// (or empty folder) is deleted when Windows next starts. NewName 0: NULL.
function MoveFileEx(ExistingName: String; NewName: Cardinal; Flags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

const
  MOVEFILE_DELAY_UNTIL_REBOOT = 4;

// What is left in Folder, files first, then their folders: deleted at the
// next restart.
procedure DeleteAtRestart(Folder: String);
var
  Find: TFindRec;
begin
  if FindFirst(Folder + '\*', Find) then
    try
      repeat
        if (Find.Name <> '.') and (Find.Name <> '..') then
          if (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            DeleteAtRestart(Folder + '\' + Find.Name)
          else
            MoveFileEx(Folder + '\' + Find.Name, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  MoveFileEx(Folder, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
end;

// The copies of WinShunTap.dll that Win顺 0.7.1 to 0.11 had Explorer load
// (the search box right of Start, since dropped): {commonappdata}\WinShun\
// tap\<hash>\. An Explorer that loaded one keeps it until it ends, and the
// file cannot be deleted meanwhile: that much goes at the next restart.
// Never a reason for Setup to fail; nothing asks for the restart.
procedure RemoveTapCopies();
var
  Folder: String;
begin
  Folder := ExpandConstant('{commonappdata}\WinShun\tap');
  if DirExists(Folder) and not DelTree(Folder, True, True, True) then
    DeleteAtRestart(Folder);
end;

// Started in the tray as the files were in place ([Run]); "run Win顺" left
// unticked on the last page, it goes again. A silent install leaves it
// running, as it was before an update.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  // The new files in place (an update that failed keeps the old version's copies).
  if CurStep = ssPostInstall then
    RemoveTapCopies();
  if (CurStep = ssDone) and not WizardSilent() and
     ((WizardForm.RunList.Items.Count = 0) or not WizardForm.RunList.Checked[0]) then
    Exec(ExpandConstant('{app}\{#AppExe}'), '--quit', '', SW_HIDE, ewNoWait, ResultCode);
end;

// Stops the index service (the service "WinShun"), so that its file can be
// replaced; waits up to 15 s for it to stop. [Run] starts it again from the
// new files. Nothing to do when it is not installed (a first install).
procedure StopIndexService();
var
  ResultCode, Waited: Integer;
begin
  if not Exec(ExpandConstant('{sys}\sc.exe'), 'stop WinShun', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    exit;
  if ResultCode <> 0 then
    exit; // not installed (1060), or not running (1062)
  Waited := 0;
  while (Waited < 15000) and Exec(ExpandConstant('{cmd}'), '/C sc query WinShun | find "STOPPED"', '', SW_HIDE,
          ewWaitUntilTerminated, ResultCode) and (ResultCode <> 0) do
  begin
    Sleep(500);
    Waited := Waited + 500;
  end;
end;

// Its autostart: the logon task WinShun creates itself. Only when it starts
// this copy; a portable copy elsewhere keeps its own. True when one went.
function RemoveAutostart(): Boolean;
var
  Listing: AnsiString;
  ListFile: String;
  ResultCode: Integer;
begin
  Result := False;
  ListFile := AddBackslash(GetTempDir()) + 'WinShun-task.txt';
  if Exec(ExpandConstant('{cmd}'), '/C schtasks /Query /TN WinShun /V /FO LIST > "' + ListFile + '"', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) and LoadStringFromFile(ListFile, Listing) then
    if Pos(Lowercase(ExpandConstant('{app}\{#AppExe}')), Lowercase(String(Listing))) > 0 then
      Result := Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN WinShun /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
        and (ResultCode = 0);
  DeleteFile(ListFile);
end;

var
  HadAutostart: Boolean;

// For [Run]: --take-autostart writes the task whether or not one is left
// when PrepareToInstall removed this copy's.
function TakeAutostartArgs(Param: String): String;
begin
  if HadAutostart then
    Result := '--had-autostart'
  else
    Result := '';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // The installed copy if there is one; a portable copy is ended.
  CloseWinShun(ExpandConstant('{app}\{#AppExe}'));
  StopIndexService();
  // This copy's autostart task of old (GitHub #21): registered elevated by
  // the version before, and by whoever answered UAC then, which for a
  // standard user was an administrator, whose task that user can neither
  // read nor replace. Removed here (elevated); [Run] has the user who
  // started Setup write their own, with normal rights.
  HadAutostart := RemoveAutostart();
  Result := '';
end;

// Win+V back to Windows' clipboard history, and Win+S (with Win+Shift+S) to
// its search, if WinShun had taken them over: the letter it put into
// Explorer's DisabledHotkeys goes (Explorer reads that when it starts). A
// letter there without WinShun having taken the key belongs to someone else.
procedure GiveBackKey(Section, Name, Key: String);
var
  Keys: String;
  I: Integer;
begin
  if CompareText(GetIniString(Section, Name, 'false', ExpandConstant('{userappdata}\WinShun\WinShun.ini')), 'true') <> 0 then
    exit;
  if not RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys', Keys) then
    exit;
  for I := Length(Keys) downto 1 do
    if Uppercase(Keys[I]) = Key then
      Delete(Keys, I, 1);
  if Keys = '' then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys')
  else
    RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys', Keys);
end;

// The folder the index was moved to in the settings ([Index] Folder), or ''
// for the default one. Read by hand: GetIniString would read the file in the
// ANSI code page, and the path may well be Chinese.
function IndexFolder(): String;
var
  Lines: TArrayOfString;
  I, Eq: Integer;
  InIndex: Boolean;
  Line: String;
begin
  Result := '';
  if not LoadStringsFromFile(ExpandConstant('{userappdata}\WinShun\WinShun.ini'), Lines) then
    exit;
  InIndex := False;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if (Length(Line) > 0) and (Line[1] = '[') then
      InIndex := CompareText(Line, '[Index]') = 0
    else if InIndex then
    begin
      Eq := Pos('=', Line);
      if (Eq > 0) and (CompareText(Trim(Copy(Line, 1, Eq - 1)), 'Folder') = 0) then
      begin
        Result := Trim(Copy(Line, Eq + 1, Length(Line)));
        if (Length(Result) >= 2) and (Result[1] = '"') and (Result[Length(Result)] = '"') then
          Result := Copy(Result, 2, Length(Result) - 2);
        StringChangeEx(Result, '/', '\', True);
        exit;
      end;
    end;
  end;
end;

// Only Win顺's own files there go (the folder itself once it is empty):
// it may be a folder the user picked.
procedure RemoveIndexFolder(Folder: String);
begin
  if (Folder = '') or not DirExists(Folder) then
    exit;
  DeleteFile(Folder + '\index.bin');
  DelTree(Folder + '\content\*.grams', False, True, False);
  DelTree(Folder + '\content\*.texts', False, True, False);
  RemoveDir(Folder + '\content');
  RemoveDir(Folder);
end;

// Explorer's right-click "用 Win顺 搜索此处" (src/app/platform/ShellMenu.cpp),
// when it runs this copy's WinShunSearch.exe: a portable copy's stays.
procedure RemoveExplorerMenu();
var
  Keys: array[0..2] of String;
  Command: String;
  I: Integer;
begin
  Keys[0] := 'Software\Classes\Directory\shell\WinShun.SearchHere';
  Keys[1] := 'Software\Classes\Directory\Background\shell\WinShun.SearchHere';
  Keys[2] := 'Software\Classes\Drive\shell\WinShun.SearchHere';
  for I := 0 to 2 do
    if RegQueryStringValue(HKCU, Keys[I] + '\command', '', Command)
      and (Pos(Lowercase(ExpandConstant('{app}\WinShunSearch.exe')), Lowercase(Command)) > 0) then
      RegDeleteKeyIncludingSubkeys(HKCU, Keys[I]);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  // Once the user has confirmed, before the files go.
  if CurUninstallStep = usUninstall then
  begin
    CloseWinShun(ExpandConstant('{app}\{#AppExe}'));
    // The index service: stopped and removed (while its file is still there).
    Exec(ExpandConstant('{app}\WinShunService.exe'), '--uninstall', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    RemoveAutostart();
    RemoveExplorerMenu();
    // Directory Opus's double click on blank space, if Win顺 set it to run this copy.
    Exec(ExpandConstant('{app}\WinShunSearch.exe'), '--opus-menu-off', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    GiveBackKey('Clipboard', 'WinV', 'V');
    GiveBackKey('Taskbar', 'WinS', 'S');
  end;
  // Old versions' copies of WinShunTap.dll (RemoveTapCopies); the folder
  // itself only when nothing else is in it (the index service's log).
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveTapCopies();
    RemoveDir(ExpandConstant('{commonappdata}\WinShun'));
  end;
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
    if MsgBox(CustomMessage('RemoveData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      RemoveIndexFolder(IndexFolder()); // before the settings that name it
      DelTree(ExpandConstant('{userappdata}\WinShun'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\WinShun'), True, True, True);
    end;
end;
