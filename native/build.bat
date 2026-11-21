@echo off
setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
set "OUT_EXE=%SCRIPT_DIR%..\VoiceDictation.exe"
set "TEST_EXE=%SCRIPT_DIR%test_native.exe"

if not defined VCINSTALLDIR (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 (
        echo [ERRO] Falha ao inicializar Visual Studio 2022 vcvars64.bat
        exit /b 1
    )
)

pushd "%SCRIPT_DIR%"
echo [1/3] Compilando e rodando testes nativos C++...
cl /nologo /std:c++20 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
   test_native.cpp audio_engine.cpp injector.cpp popup_card.cpp ^
   /Fe:"%TEST_EXE%" ^
   user32.lib gdi32.lib gdiplus.lib winmm.lib advapi32.lib shell32.lib
if errorlevel 1 (
    popd
    exit /b 1
)
"%TEST_EXE%"
if errorlevel 1 (
    popd
    exit /b 1
)

echo [2/3] Compilando VoiceDictation.exe (Release x64 /MT, zero dependencias Python)...
cl /nologo /std:c++20 /O2 /GL /Gy /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
   main.cpp audio_engine.cpp net_client.cpp injector.cpp settings_ui.cpp popup_card.cpp ^
   /Fe:"%OUT_EXE%" ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib gdiplus.lib winmm.lib winhttp.lib dwmapi.lib shell32.lib ole32.lib advapi32.lib
if errorlevel 1 (
    popd
    exit /b 1
)

echo [3/3] Limpando objetos intermediarios...
del /q *.obj *.pdb 2>nul
popd
echo [OK] Build concluido: "%OUT_EXE%"
exit /b 0
