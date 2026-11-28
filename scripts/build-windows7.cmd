@echo off
setlocal
chcp 65001 >nul
if not defined NAVIDOG_QT_ROOT (
  echo Please set NAVIDOG_QT_ROOT to Qt 5.15.2 mingw81_64.
  exit /b 1
)
if not defined NAVIDOG_MINGW_ROOT (
  echo Please set NAVIDOG_MINGW_ROOT to the Qt MinGW 8.1 x64 toolchain.
  exit /b 1
)
if not defined NAVIDOG_SQLITE_SOURCE (
  echo Please set NAVIDOG_SQLITE_SOURCE to the SQLite amalgamation source directory.
  exit /b 1
)
set "WINDOWS_VERSION=%~1"
if not defined WINDOWS_VERSION set "WINDOWS_VERSION=7"
if not "%WINDOWS_VERSION%"=="7" if not "%WINDOWS_VERSION%"=="8" exit /b 2
set "PATH=%NAVIDOG_MINGW_ROOT%\bin;%NAVIDOG_QT_ROOT%\bin;%PATH%"
set "PROJECT_ROOT=%~dp0.."
cmake -S "%PROJECT_ROOT%" -B "%PROJECT_ROOT%\build-win%WINDOWS_VERSION%-native" -G "MinGW Makefiles" -DNAVIDOG_WIN7=ON -DNAVIDOG_LEGACY_WINDOWS_VERSION=%WINDOWS_VERSION% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%NAVIDOG_QT_ROOT%" -DNAVIDOG_SQLITE_SOURCE="%NAVIDOG_SQLITE_SOURCE%" -DNAVIDOG_WITH_POSTGRES=OFF -DNAVIDOG_WITH_MYSQL=OFF -DNAVIDOG_WITH_ORACLE=OFF -DNAVIDOG_WITH_SQLSERVER=OFF -DNAVIDOG_BUILD_TESTS=OFF -DBUILD_TESTING=OFF
if errorlevel 1 exit /b 1
cmake --build "%PROJECT_ROOT%\build-win%WINDOWS_VERSION%-native" --target navidog --parallel 2
if errorlevel 1 exit /b 1
set "DIST=%PROJECT_ROOT%\dist\Navidog-Win%WINDOWS_VERSION%-x64"
if not exist "%DIST%" mkdir "%DIST%"
copy /y "%PROJECT_ROOT%\build-win%WINDOWS_VERSION%-native\navidog.exe" "%DIST%\navidog.exe" >nul
if exist "%NAVIDOG_QT_ROOT%\bin\windeployqt.exe" (
  "%NAVIDOG_QT_ROOT%\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime "%DIST%\navidog.exe"
  if errorlevel 1 exit /b 1
) else (
  echo Qt deployment tool unavailable; copying the native Widgets dependencies.
  for %%f in (Qt5Core.dll Qt5Gui.dll Qt5Widgets.dll Qt5Network.dll) do (
    copy /y "%NAVIDOG_QT_ROOT%\bin\%%f" "%DIST%\%%f" >nul
    if errorlevel 1 exit /b 1
  )
  if not exist "%DIST%\platforms" mkdir "%DIST%\platforms"
  copy /y "%NAVIDOG_QT_ROOT%\plugins\platforms\qwindows.dll" "%DIST%\platforms\qwindows.dll" >nul
  if errorlevel 1 exit /b 1
  if exist "%NAVIDOG_QT_ROOT%\plugins\styles\qwindowsvistastyle.dll" (
    if not exist "%DIST%\styles" mkdir "%DIST%\styles"
    copy /y "%NAVIDOG_QT_ROOT%\plugins\styles\qwindowsvistastyle.dll" "%DIST%\styles\qwindowsvistastyle.dll" >nul
    if errorlevel 1 exit /b 1
  )
)
if exist "%NAVIDOG_QT_ROOT%\translations\qtbase_zh_CN.qm" (
  if not exist "%DIST%\translations" mkdir "%DIST%\translations"
  copy /y "%NAVIDOG_QT_ROOT%\translations\qtbase_zh_CN.qm" "%DIST%\translations\qtbase_zh_CN.qm" >nul
)
echo [Paths]>"%DIST%\qt.conf"
echo Plugins=.>>"%DIST%\qt.conf"
for %%f in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do copy /y "%NAVIDOG_MINGW_ROOT%\bin\%%f" "%DIST%\%%f" >nul
if not exist "%DIST%\licenses" mkdir "%DIST%\licenses"
for %%f in (LICENSE.LGPLv3 LICENSE.GPLv3 LICENSE.FDL) do if exist "%NAVIDOG_QT_ROOT%\%%f" copy /y "%NAVIDOG_QT_ROOT%\%%f" "%DIST%\licenses\%%f" >nul
for %%d in (gcc winpthreads mingw-w64) do xcopy /e /i /y "%NAVIDOG_MINGW_ROOT%\licenses\%%d" "%DIST%\licenses\%%d" >nul
echo Built: %DIST%\navidog.exe
echo This legacy edition includes SQLite. Validate on Windows %WINDOWS_VERSION% x64 before distributing.
