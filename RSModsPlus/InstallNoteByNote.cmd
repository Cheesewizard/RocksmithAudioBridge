@echo off
setlocal
echo Close Rocksmith before installing the Note by Note menu.
start "" /wait "%~dp0RSMods.exe" --install-note-by-note-menu "%~dp0."
if errorlevel 1 (
 echo Installation failed. See NoteByNoteMenuInstall.log and the startup log in LocalAppData\RSModsPlus\Logs.
 pause
 exit /b 1
)
echo Note by Note is installed in Riff Repeater Advanced Settings.
pause
