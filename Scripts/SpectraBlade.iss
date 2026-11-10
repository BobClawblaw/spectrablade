; The Windows installer, built by Scripts/package-windows.ps1 with Inno Setup 6:
;   ISCC /DAppVersion=<version> /DStage=<folder with SpectraBlade.exe and the licences>
;        /DIconFile=<the app's .ico> /DOutputDir=<where the installer goes> Scripts\SpectraBlade.iss
;
; Installs for the current user (no administrator rights), or for everyone when asked to (/ALLUSERS, or
; the choice Setup offers an administrator). It adds a
; Start menu entry, puts SpectraBlade in Explorer's Open With list for the audio files it opens
; (AudioFileIO::openedFromFinder), and, as a task, opens sessions and projects with it.

#ifndef AppVersion
  #error AppVersion must be given (/DAppVersion=...)
#endif
#ifndef Stage
  #error Stage must be given (/DStage=...)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define AppExe "SpectraBlade.exe"

[Setup]
AppId={{82351F7F-F7FC-4F48-B875-D9356B09DE47}
AppName=SpectraBlade
AppVersion={#AppVersion}
AppVerName=SpectraBlade {#AppVersion}
AppPublisher=SpectraBlade
AppPublisherURL=https://github.com/BobClawblaw/spectrablade
AppSupportURL=https://github.com/BobClawblaw/spectrablade/issues
AppUpdatesURL=https://github.com/BobClawblaw/spectrablade/releases
DefaultDirName={autopf}\SpectraBlade
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#Stage}\LICENSE.txt
OutputDir={#OutputDir}
OutputBaseFilename=SpectraBlade-{#AppVersion}-windows-x64-setup
#ifdef IconFile
SetupIconFile={#IconFile}
#endif
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Open SpectraBlade sessions and projects (.sbsession, .sbproject) with SpectraBlade"; GroupDescription: "File types:"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\SpectraBlade"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\SpectraBlade"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; The kinds of file SpectraBlade opens, each opening with it.
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Audio"; ValueType: string; ValueData: "Audio file"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Audio\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Audio\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Session"; ValueType: string; ValueData: "SpectraBlade Session"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Session\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Session\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Project"; ValueType: string; ValueData: "SpectraBlade Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Project\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\SpectraBlade.Project\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""

; The extensions' keys: taken away on uninstalling only if nothing else is left in them (another
; program's entries keep them). Listed before what goes in them, as uninstalling works last first.
Root: HKA; Subkey: "Software\Classes\.sbsession"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.sbproject"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.wav"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.aif"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.aiff"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.flac"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.ogg"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.mp3"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.m4a"; Flags: uninsdeletekeyifempty

; Sessions and projects open with SpectraBlade, if chosen; it is in their Open With list either way.
Root: HKA; Subkey: "Software\Classes\.sbsession"; ValueType: string; ValueName: ""; ValueData: "SpectraBlade.Session"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.sbproject"; ValueType: string; ValueName: ""; ValueData: "SpectraBlade.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.sbsession\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Session"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.sbproject\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Project"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty

; Audio files keep whatever opens them now: SpectraBlade is offered in their Open With list.
Root: HKA; Subkey: "Software\Classes\.wav\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.aif\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.aiff\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.flac\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.ogg\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.mp3\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.m4a\OpenWithProgids"; ValueType: string; ValueName: "SpectraBlade.Audio"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty

; The program itself: its name in Open With, the types it takes, and "SpectraBlade" in Run (Win+R).
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "SpectraBlade"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".wav"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".aif"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".aiff"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".flac"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".ogg"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".mp3"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".m4a"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".sbsession"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".sbproject"; ValueData: ""
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\{#AppExe}"; ValueType: string; ValueData: "{app}\{#AppExe}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,SpectraBlade}"; Flags: nowait postinstall skipifsilent
