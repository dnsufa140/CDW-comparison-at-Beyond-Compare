@echo off
chcp 65001 >nul
setlocal

echo Сборка cdw2png.exe с помощью MSVC (cl.exe)...
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE main.cpp /Fe:cdw2png.exe ole32.lib oleaut32.lib uuid.lib shell32.lib

if %ERRORLEVEL% NEQ 0 (
    echo Ошибка сборки! Запустите скрипт из "x64 Native Tools Command Prompt for VS".
    exit /b %ERRORLEVEL%
)

del /q main.obj 2>nul
echo Готово: cdw2png.exe
endlocal
