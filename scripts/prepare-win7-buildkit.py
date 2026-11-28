#!/usr/bin/env python3
"""Assemble an offline Windows build kit from the locally installed SDK."""
from pathlib import Path
import shutil
import stat
import re
root=Path(__file__).resolve().parents[1]
sdk=Path.home()/'Library/Developer/Navidog'
kit=sdk/'Win7-BuildKit'
kit.mkdir(exist_ok=True)
version=re.search(r'project\(Navidog VERSION ([0-9.]+)',(root/'CMakeLists.txt').read_text()).group(1)
def copytree(src,dest):
 def copy_file(a,b):
  target=Path(b)
  if target.exists():
   target.chmod(target.stat().st_mode | stat.S_IWUSR)
  shutil.copy2(a,b)
  target.chmod(target.stat().st_mode | stat.S_IWUSR)
  return b
 shutil.copytree(src,dest,dirs_exist_ok=True,copy_function=copy_file,ignore=shutil.ignore_patterns('.DS_Store','__pycache__'))
for name in ('apps','src','include','tests','cmake','docs','scripts','resources'):
 copytree(root/name,kit/'Source'/name)
for name in ('CMakeLists.txt','README.md','AGENTS.md'):
 shutil.copy2(root/name,kit/'Source'/name)
copytree(sdk/'Qt/5.15.2/mingw81_64',kit/'Qt/5.15.2/mingw81_64')
copytree(sdk/'Qt/Tools/mingw810_64',kit/'Tools/mingw810_64')
copytree(sdk/'win7-deps/cmake-3.24.4-windows-x86_64',kit/'Tools/cmake-3.24.4-windows-x86_64')
copytree(sdk/'win7-deps/sqlite-amalgamation-3510000',kit/'sqlite')
copytree(root/'dist/Navidog-Win7-x64',kit/'Prebuilt/Navidog-Win7-x64')
(kit/'Qt/5.15.2/mingw81_64/bin/qt.conf').write_text('[Paths]\nPrefix=..\n')
(kit/'build.cmd').write_bytes('''@echo off
setlocal
set "KIT_ROOT=%~dp0"
if not exist "%KIT_ROOT%Source\\CMakeLists.txt" exit /b 1
set "NAVIDOG_QT_ROOT=%KIT_ROOT%Qt\\5.15.2\\mingw81_64"
set "NAVIDOG_MINGW_ROOT=%KIT_ROOT%Tools\\mingw810_64"
set "NAVIDOG_SQLITE_SOURCE=%KIT_ROOT%sqlite"
set "PATH=%KIT_ROOT%Tools\\cmake-3.24.4-windows-x86_64\\bin;%PATH%"
call "%KIT_ROOT%Source\\scripts\\build-windows7.cmd"
set "BUILD_RESULT=%ERRORLEVEL%"
echo Build exit code: %BUILD_RESULT%
pause
exit /b %BUILD_RESULT%
'''.replace('\n','\r\n').encode())
(kit/'setup.cmd').write_bytes('''@echo off
setlocal
echo Copying the offline build environment to C:\\NavidogBuild...
if not exist C:\\NavidogBuild mkdir C:\\NavidogBuild
xcopy /e /i /y "%~dp0*" C:\\NavidogBuild\\ > C:\\NavidogBuild\\setup.log 2>&1
if errorlevel 1 (
 echo Copy failed. See C:\\NavidogBuild\\setup.log
 pause
 exit /b 1
)
attrib -r C:\\NavidogBuild\\Source\\* /s
echo Environment copied. Starting the prebuilt SQLite test edition.
start "" C:\\NavidogBuild\\Prebuilt\\Navidog-Win7-x64\\navidog.exe
echo To compile, run C:\\NavidogBuild\\build.cmd
pause
'''.replace('\n','\r\n').encode())
(kit/'README.txt').write_text(f'Navidog {version} Windows 7 SP1 x64 offline build kit\n\nCopy ALL files from this disc to a writable local directory, e.g. C:\\NavidogBuild.\nDo not build from the read-only CD drive. Run build.cmd to compile.\nPrebuilt\\Navidog-Win7-x64\\navidog.exe is the cross-built SQLite-only test edition.\nQt 5.15.2 / MinGW 8.1 x64 / CMake 3.24.4 / SQLite 3.51.0.\nSource snapshot contains no database passwords or workspace profiles.\nThe current source and prebuilt snapshot is {version}. Its cross build and PE dependency audit passed. The last VM runtime acceptance was 0.16; the current snapshot has not yet passed a fresh VM runtime or native build. Earlier 0.5 was compiled inside the VM. Full regression and physical-machine acceptance remain pending.\nThis kit does not include Windows installation media, license keys or activation tools.\n')
print(kit)
