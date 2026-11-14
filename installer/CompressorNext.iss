; Setup für Compressor NEXT (Inno Setup 6)
; Bauen: build.ps1 im Hauptordner (erst nativ + dotnet publish, dann ISCC).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{81B591A8-925D-4D8D-BEAC-B4B7B6A8E880}
AppName=Compressor NEXT
AppVersion={#AppVersion}
AppVerName=Compressor NEXT {#AppVersion}
AppPublisher=ShakieVan
DefaultDirName={autopf}\Compressor NEXT
DefaultGroupName=Compressor NEXT
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=CompressorNext-Setup-{#AppVersion}
SetupIconFile=..\src\CompressorNext\app.ico
UninstallDisplayIcon={app}\CompressorNext.exe
UninstallDisplayName=Compressor NEXT
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Die laufende Instanz wird selbst beendet (CompressorNext.exe --shutdown).
CloseApplications=no

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[Tasks]
Name: "desktopicon"; Description: "Verknüpfung auf dem Desktop"; Flags: unchecked

[Files]
Source: "..\publish\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Compressor NEXT"; Filename: "{app}\CompressorNext.exe"
Name: "{autodesktop}\Compressor NEXT"; Filename: "{app}\CompressorNext.exe"; Tasks: desktopicon

[Run]
; Als angemeldeter Benutzer (nicht als Administrator) starten: richtet den Autostart für ihn ein und zeigt das Fenster.
Filename: "{app}\CompressorNext.exe"; Parameters: "--first-run"; Flags: nowait runasoriginaluser; StatusMsg: "Compressor NEXT wird gestartet …"

[Code]
const
  RuntimeUrl = 'https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe';
  RuntimeFile = 'windowsdesktop-runtime-10-win-x64.exe';

var
  DownloadPage: TDownloadWizardPage;

function IsDesktopRuntimeInstalled: Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App\10.*'), FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          Result := True;
      until Result or not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ Prüft, ob die heruntergeladene Datei gültig von Microsoft signiert ist. }
function IsSignedByMicrosoft(FileName: String): Boolean;
var
  ResultCode: Integer;
  Command: String;
begin
  Command := '-NoProfile -ExecutionPolicy Bypass -Command "$s = Get-AuthenticodeSignature -LiteralPath ''' + FileName + '''; ' +
             'if ($s.Status -eq ''Valid'' -and $s.SignerCertificate.Subject -like ''*O=Microsoft Corporation*'') { exit 0 } else { exit 1 }"';
  Result := Exec('powershell.exe', Command, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InstallRuntime: Boolean;
var
  FileName: String;
  ResultCode: Integer;
begin
  Result := False;
  FileName := ExpandConstant('{tmp}\' + RuntimeFile);
  if not IsSignedByMicrosoft(FileName) then
  begin
    MsgBox('Die heruntergeladene Datei ist nicht gültig von Microsoft signiert. Die Installation wird abgebrochen.', mbCriticalError, MB_OK);
    Exit;
  end;
  DownloadPage.SetText('Die .NET 10 Desktop Runtime wird installiert …', '');
  if Exec(FileName, '/install /quiet /norestart', '', SW_SHOW, ewWaitUntilTerminated, ResultCode)
     and ((ResultCode = 0) or (ResultCode = 1641) or (ResultCode = 3010)) then
    Result := True
  else
    MsgBox('Die .NET 10 Desktop Runtime konnte nicht installiert werden (Code ' + IntToStr(ResultCode) + ').', mbError, MB_OK);
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), nil);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID <> wpReady) or IsDesktopRuntimeInstalled then
    Exit;

  if MsgBox('Die Bedienoberfläche von Compressor NEXT braucht die .NET 10 Desktop Runtime von Microsoft. Sie ist auf diesem Rechner nicht installiert.' + #13#10#13#10 +
            'Jetzt von Microsoft herunterladen (ca. 60 MB) und installieren?', mbConfirmation, MB_YESNO) <> IDYES then
  begin
    MsgBox('Ohne die .NET 10 Desktop Runtime startet die Bedienoberfläche nicht.' + #13#10 +
           'Download: https://dotnet.microsoft.com/download/dotnet/10.0 („.NET Desktop Runtime“, x64)', mbInformation, MB_OK);
    Result := False;
    Exit;
  end;

  DownloadPage.Clear;
  DownloadPage.Add(RuntimeUrl, RuntimeFile, '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      Result := InstallRuntime;
    except
      if DownloadPage.AbortedByUser then
        Log('Download abgebrochen.')
      else
        SuppressibleMsgBox(AddPeriod(GetExceptionMessage), mbCriticalError, MB_OK, IDOK);
      Result := False;
    end;
  finally
    DownloadPage.Hide;
  end;
end;

procedure StopRunningApp;
var
  ResultCode: Integer;
begin
  if FileExists(ExpandConstant('{app}\CompressorNext.exe')) then
    Exec(ExpandConstant('{app}\CompressorNext.exe'), '--shutdown', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  { Falls die Oberfläche unter einem anderen Konto läuft und das Signal nicht ankam. }
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM CompressorNext.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

{ Bei einem Update: Oberfläche beenden, Dienst anhalten und das Audio-Modul aushängen, damit sich die Dateien ersetzen lassen. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  StopRunningApp;
  if FileExists(ExpandConstant('{app}\CompressorNextSvc.exe')) then
    Exec(ExpandConstant('{app}\CompressorNextSvc.exe'), 'detach', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

{ Nach dem Kopieren: Audio-Modul registrieren, in alle Ausgabegeräte einbinden, Dienst einrichten. }
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;
  WizardForm.StatusLabel.Caption := 'Audio-Modul wird eingerichtet (der Ton setzt kurz aus) …';
  if not Exec(ExpandConstant('{app}\CompressorNextSvc.exe'), 'install', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    SuppressibleMsgBox('Das Audio-Modul konnte nicht vollständig eingerichtet werden (Code ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
                       'Einzelheiten stehen in ' + ExpandConstant('{app}\service.log') + '.' + #13#10 +
                       'In Compressor NEXT lässt sich die Einrichtung mit „Beheben“ wiederholen.', mbError, MB_OK, IDOK);
  { Entwicklung: Setup.exe /SILENT /probe=1 probiert danach jeden Effektplatz einzeln durch (siehe service.log). }
  // /probe=CLSID (mit geschweiften Klammern) stellt dabei zusätzlich einen Treiber mit eigenem Stream-Modul nach.
  if ExpandConstant('{param:probe|0}') = '1' then
    Exec(ExpandConstant('{app}\CompressorNextSvc.exe'), 'probe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
  else if ExpandConstant('{param:probe|0}') <> '0' then
    Exec(ExpandConstant('{app}\CompressorNextSvc.exe'), 'probe ' + ExpandConstant('{param:probe|0}'), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

{ Deinstallation: Oberfläche beenden, Autostart entfernen, Audio-Modul überall aushängen und abmelden. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep <> usUninstall then
    Exit;
  StopRunningApp;
  Exec(ExpandConstant('{app}\CompressorNext.exe'), '--remove-autostart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if not Exec(ExpandConstant('{app}\CompressorNextSvc.exe'), 'uninstall', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Log('CompressorNextSvc.exe uninstall konnte nicht gestartet werden.');
end;
