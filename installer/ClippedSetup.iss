; Script de Inno Setup para Clipped (Instalador ligero para Windows 10/11 x64)
#define MyAppName      "Clipped"
#define MyAppVersion   "1.0.0"
#define MyAppPublisher "Clipped"
#define MyAppExeName   "Clipped.exe"

[Setup]
AppId={{8F4B9A2E-5C1D-4E7A-9B3C-6D2E1F0A8C4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=ClippedSetup
SetupIconFile=..\src\clip.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=classic
CloseApplications=force

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "startup"; Description: "Iniciar {#MyAppName} automáticamente con Windows (en segundo plano)"; GroupDescription: "Opciones de inicio:"

[Files]
Source: "..\build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Historial ligero de portapapeles (Ctrl + Alt + V)"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Clipped"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startup

[Run]
; Al terminar de instalar, arranca inmediatamente en segundo plano en el System Tray
Filename: "{app}\{#MyAppExeName}"; Parameters: "--first-run"; Description: "Ejecutar {#MyAppName} ahora en segundo plano (System Tray)"; Flags: nowait postinstall

[UninstallRun]
; Cerrar Clipped en segundo plano antes de desinstalar
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden; RunOnceId: "StopClippedOnUninstall"
