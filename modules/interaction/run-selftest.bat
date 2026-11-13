@echo off
chcp 65001 >nul
setlocal
set "QT=D:\qtcreator"
set "PROJ=D:\QtProject\QtDocumentEditor"
set "PATH=%QT%\Tools\mingw1510_64\bin;%QT%\Tools\CMake_64\bin;%QT%\Tools\Ninja;%QT%\6.12.0\mingw_64\bin;%PATH%"
set "PREFIX=%QT%/6.12.0/mingw_64"
set "CXX=%QT%/Tools/mingw1510_64/bin/g++.exe"

echo [1/2] Build interaction module...
cmake -S "%PROJ%" -B "%PROJ%\build\v01" -G Ninja -DCMAKE_PREFIX_PATH="%PREFIX%" -DCMAKE_CXX_COMPILER="%CXX%" >nul
if errorlevel 1 goto fail
cmake --build "%PROJ%\build\v01" --target InteractionSelfCheck
if errorlevel 1 goto fail

echo [2/2] Run InteractionSelfCheck...
"%PROJ%\build\v01\modules\interaction\InteractionSelfCheck.exe"
if errorlevel 1 goto fail

echo.
echo [OK] Interaction self-check passed.
echo.
if not defined SOLO_NO_PAUSE pause
exit /b 0

:fail
echo.
echo [FAILED] Send me the lines above.
if not defined SOLO_NO_PAUSE pause
exit /b 1
