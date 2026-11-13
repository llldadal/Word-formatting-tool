@echo off
chcp 65001 >nul
setlocal
set "QT=D:\qtcreator"
set "PROJ=D:\QtProject\QtDocumentEditor"
set "PATH=%QT%\Tools\mingw1510_64\bin;%QT%\Tools\CMake_64\bin;%QT%\Tools\Ninja;%QT%\6.12.0\mingw_64\bin;%PATH%"
set "PREFIX=%QT%/6.12.0/mingw_64"
set "CXX=%QT%/Tools/mingw1510_64/bin/g++.exe"

echo [1/2] Build EditorDemo...
cmake -S "%PROJ%" -B "%PROJ%\build\v01" -G Ninja -DCMAKE_PREFIX_PATH="%PREFIX%" -DCMAKE_CXX_COMPILER="%CXX%" >nul
if errorlevel 1 goto fail
cmake --build "%PROJ%\build\v01" --target EditorDemo
if errorlevel 1 goto fail

echo [2/2] Launch the editor window (close the window to exit)...
"%PROJ%\build\v01\modules\io\EditorDemo.exe"
if errorlevel 1 goto fail

exit /b 0

:fail
echo.
echo [FAILED] Send me the lines above.
pause
exit /b 1
