@echo off
setlocal
cd /d "%~dp0"

set "APP_EXE=%~dp0VoiceDictation.exe"
if not exist "%APP_EXE%" goto FAIL

if /i "%~1"=="--silent" goto SILENT
if /i "%~1"=="--background" goto SILENT

:: Modo normal (clique no atalho):
:: Se ja estiver rodando, o proprio VoiceDictation.exe sinaliza VoiceDictation_Toggle_Event e sai em <5ms.
:: Caso contrario, inicia o app diretamente (ou via tarefa agendada) e mostra a janela.
start "" "%APP_EXE%"
exit /b 0

:SILENT
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_live.ps1" -Silent >> "%~dp0launch_err.log" 2>&1
exit /b 0

:FAIL
powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [void][System.Windows.Forms.MessageBox]::Show('VoiceDictation.exe nao encontrado em %~dp0','Voice Dictation',0,16)" >nul 2>&1
exit /b 1
