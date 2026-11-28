set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH "$ENV{NAVIDOG_WIN_QT}" "$ENV{NAVIDOG_WIN_DEPS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -Wl,--major-subsystem-version,6,--minor-subsystem-version,1")
# Homebrew's modern MinGW defaults to UCRT. For Win7 use the official Qt-era
# MinGW 8 runtime/start files and C++ headers, without replacing the host compiler.
if(DEFINED ENV{NAVIDOG_LEGACY_MINGW_ROOT})
    file(TO_CMAKE_PATH "$ENV{NAVIDOG_LEGACY_MINGW_ROOT}" legacy)
    set(legacy_gcc "${legacy}/lib/gcc/x86_64-w64-mingw32/8.1.0")
    # Override only start/end objects; -B would also replace compiler intrinsic
    # headers with incompatible GCC 8 intrinsics under the modern host compiler.
    set(specs "${CMAKE_BINARY_DIR}/navidog-msvcrt.specs")
    file(WRITE "${specs}" "*startfile:\n${legacy}/x86_64-w64-mingw32/lib/crt2.o ${legacy_gcc}/crtbegin.o\n\n*endfile:\n${legacy_gcc}/crtend.o\n\n")
    set(crt_flags "-mcrtdll=msvcrt -specs=\"${specs}\" -L\"${legacy_gcc}\" -L\"${legacy}/x86_64-w64-mingw32/lib\" -L\"${legacy}/lib\"")
    set(CMAKE_C_FLAGS_INIT "${crt_flags}")
    set(CMAKE_CXX_FLAGS_INIT "${crt_flags} -nostdinc++ -isystem \"${legacy_gcc}/include/c++\" -isystem \"${legacy_gcc}/include/c++/x86_64-w64-mingw32\" -isystem \"${legacy_gcc}/include/c++/backward\"")
endif()
