#!/usr/bin/env python3
"""Package a legacy Windows build and audit its PE dependencies."""
import hashlib,json,os,pathlib,re,shutil,subprocess,sys,zipfile
root=pathlib.Path(__file__).resolve().parents[1]
version=re.search(r'project\(Navidog VERSION ([0-9.]+)',(root/'CMakeLists.txt').read_text()).group(1)
binary,qt,mingw=map(pathlib.Path,sys.argv[1:4])
windows=sys.argv[4] if len(sys.argv)>4 else '7'
if windows not in ('7','8'):
    raise SystemExit('Expected a Windows target of 7 or 8')
platform=f'Win{windows}-x64'
objdump=str(mingw/'bin'/'objdump.exe') if os.name=='nt' else 'x86_64-w64-mingw32-objdump'
stage=root/'dist'/f'Navidog-{platform}'
if stage.exists():shutil.rmtree(stage)
stage.mkdir(parents=True)
shutil.copy2(binary,stage/'navidog.exe')
for name in ['Qt5Core.dll','Qt5Gui.dll','Qt5Widgets.dll','Qt5Network.dll']:
    shutil.copy2(qt/'bin'/name,stage/name)
for name in ['libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll']:
    shutil.copy2(mingw/'bin'/name,stage/name)
for name in ['platforms/qwindows.dll','styles/qwindowsvistastyle.dll']:
    src=qt/'plugins'/name
    if src.exists():
        dest=stage/name;dest.parent.mkdir(exist_ok=True);shutil.copy2(src,dest)
translation=qt/'translations'/'qtbase_zh_CN.qm'
if translation.exists():
    (stage/'translations').mkdir(exist_ok=True);shutil.copy2(translation,stage/'translations'/translation.name)
(stage/'qt.conf').write_text('[Paths]\nPlugins=.\n')
licenses=stage/'licenses';licenses.mkdir()
shutil.copy2(root/'LICENSE', licenses/'Navidog-LICENSE')
shutil.copy2(root/'NOTICE', licenses/'Navidog-NOTICE')
for name in ['gcc','winpthreads','mingw-w64']:
    src=mingw/'licenses'/name
    if src.is_dir():shutil.copytree(src,licenses/name)
for name in ['LICENSE.LGPLv3','LICENSE.GPLv3','LICENSE.FDL','LICENSE.GPL2','LICENSE.GPL3']:
    src=qt/name
    if src.exists():shutil.copy2(src,licenses/name)
(licenses/'THIRD-PARTY.txt').write_text('Qt 5.15.2: LGPLv3 / GPLv3; dynamically linked, replaceable DLLs.\nSources and license: https://download.qt.io/archive/qt/5.15/5.15.2/submodules/\nMinGW-w64 / GCC 8.1 runtime: GCC runtime library exception and MinGW licenses.\nhttps://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html\nSQLite 3.51.0: public domain; https://www.sqlite.org/copyright.html\n')
system={'kernel32.dll','msvcrt.dll','user32.dll','gdi32.dll','advapi32.dll','shell32.dll','ole32.dll','oleaut32.dll','uuid.dll','comdlg32.dll','comctl32.dll','winmm.dll','ws2_32.dll','imm32.dll','version.dll','uxtheme.dll','dwmapi.dll','shlwapi.dll','crypt32.dll','secur32.dll','netapi32.dll','userenv.dll','wtsapi32.dll','iphlpapi.dll','mpr.dll','winspool.drv','dnsapi.dll','setupapi.dll','opengl32.dll','glu32.dll','rpcrt4.dll','msimg32.dll','d3d9.dll','dxgi.dll','d3d11.dll','dxguid.dll','bcrypt.dll','normaliz.dll','powrprof.dll','hid.dll','wintrust.dll','wininet.dll'}
available={p.name.lower() for p in stage.rglob('*') if p.suffix.lower() in ['.dll','.exe']};audit={}
for path in stage.rglob('*'):
    if path.suffix.lower() not in ['.dll','.exe']:continue
    text=subprocess.check_output([objdump,'-p',str(path)],text=True)
    if path==stage/'navidog.exe':
        major=re.search(r'^MajorSubsystemVersion\s+(\d+)',text,re.MULTILINE)
        minor=re.search(r'^MinorSubsystemVersion\s+(\d+)',text,re.MULTILINE)
        if not major or not minor or (int(major[1]),int(minor[1]))!=(6,int(windows)-6):
            raise SystemExit(f'Unexpected PE subsystem version for Windows {windows}')
    deps=re.findall(r'DLL Name:\s*(\S+)',text);audit[str(path.relative_to(stage))]=deps
    for dep in deps:
        d=dep.lower()
        if d.startswith('api-ms-win-crt-') or d=='ucrtbase.dll':raise SystemExit('Unexpected UCRT dependency: '+dep)
        if d not in available and d not in system:raise SystemExit('Unpackaged dependency: '+dep)
(stage/'dependency-audit.json').write_text(json.dumps(audit,indent=2))
(stage/'README.txt').write_text(f'Navidog {version} - Windows {windows} x64 compatibility build\n\nExtract the entire ZIP to a folder, then run navidog.exe. Do not copy only the EXE.\nThis initial legacy edition includes SQLite only. It does not include the Mac edition remote drivers.\nBuilt with Qt 5.15.2 and the legacy MSVCRT runtime; no UCRT package is required by its imports.\nRun this build on the target Windows version before deploying it. Windows 7 requires SP1.\nUse the included sample database first. Query and table/data editing use the same native UI.\nSource/build instructions: BUILDING.md in the project.\n')
output=root/'dist'/f'Navidog-{version}-{platform}-Portable.zip'
with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for path in stage.rglob('*'):
        if path.is_file():z.write(path,path.relative_to(stage.parent))
digest=hashlib.sha256(output.read_bytes()).hexdigest();output.with_suffix('.zip.sha256').write_text(digest+'  '+output.name+'\n')
print(output);print('SHA256:',digest);print('Audited PE files:',len(audit))
