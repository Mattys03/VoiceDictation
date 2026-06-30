@echo off
chcp 65001 >nul
title Voice Dictation - Instalador

echo ╔══════════════════════════════════════════════╗
echo ║   Voice Dictation – Instalador (Windows)     ║
echo ╚══════════════════════════════════════════════╝
echo.

:: ── Check Python ──
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [X] Python nao encontrado!
    echo.
    echo     Baixe e instale o Python em:
    echo     https://www.python.org/downloads/
    echo.
    echo     IMPORTANTE: Marque "Add Python to PATH" durante a instalacao!
    echo.
    pause
    exit /b 1
)

echo [OK] Python encontrado:
python --version
echo.

:: ── Install dependencies ──
echo [..] Instalando dependencias...
pip install -r "%~dp0requirements.txt"
if %errorlevel% neq 0 (
    echo [X] Erro ao instalar dependencias.
    echo     Tente executar como Administrador.
    pause
    exit /b 1
)
echo [OK] Dependencias instaladas!
echo.

:: ── Create Desktop shortcut ──
echo [..] Criando atalho na Area de Trabalho...
set SCRIPT_PATH=%~dp0voice_dictation.py
set SHORTCUT_PATH=%USERPROFILE%\Desktop\Voice Dictation.lnk

powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut('%SHORTCUT_PATH%'); $s.TargetPath = 'pythonw'; $s.Arguments = '\"%SCRIPT_PATH%\"'; $s.WorkingDirectory = '%~dp0'; $s.Description = 'Voice Dictation - Alt+H para ditar'; $s.Save()"

if exist "%SHORTCUT_PATH%" (
    echo [OK] Atalho criado na Area de Trabalho!
) else (
    echo [!] Nao foi possivel criar o atalho. Crie manualmente.
)
echo.

:: ── Create Startup shortcut (autostart) ──
echo [..] Configurando inicializacao automatica...
set STARTUP_PATH=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Voice Dictation.lnk

powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut('%STARTUP_PATH%'); $s.TargetPath = 'pythonw'; $s.Arguments = '\"%SCRIPT_PATH%\"'; $s.WorkingDirectory = '%~dp0'; $s.Description = 'Voice Dictation - Autostart'; $s.WindowStyle = 7; $s.Save()"

if exist "%STARTUP_PATH%" (
    echo [OK] Inicializacao automatica configurada!
) else (
    echo [!] Nao foi possivel configurar autostart.
)

echo.
echo ═══════════════════════════════════════════════
echo   Instalacao concluida!
echo.
echo   • Alt+H  → Ativar/desativar ditado por voz
echo   • O programa inicia automaticamente com o Windows
echo   • Requer conexao com a internet
echo.
echo   Deseja iniciar agora? (S/N)
echo ═══════════════════════════════════════════════

set /p INICIAR="> "
if /i "%INICIAR%"=="S" (
    echo.
    echo [OK] Iniciando Voice Dictation...
    start "" pythonw "%SCRIPT_PATH%"
)

echo.
echo Pressione qualquer tecla para sair...
pause >nul
