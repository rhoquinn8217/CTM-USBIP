@echo off
REM Builds the DS4 tone generator. Standalone on purpose: running it must not
REM mean rebuilding the listener, which would mean releasing a bridged pad.
REM
REM !! RELATIVE PATHS AFTER pushd, DELIBERATELY. The repo lives under a folder
REM !! with a space in it ("New folder"), and cl parses /Fe"<path>" by splitting
REM !! at the quote rather than honouring it -- so an absolute path here fails
REM !! with "unrecognized source file type" and a link error naming half the
REM !! path. Relative paths have no spaces and need no quotes at all.
setlocal
pushd "%~dp0"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: could not find vcvars64.bat
    popd
    exit /b 1
)

cl /nologo /EHsc /O2 /std:c++17 ^
   /I..\..\third_party\ffmpeg\x64\release\include ^
   /Fods4-tone-gen.obj /Feds4-tone-gen.exe ^
   ds4-tone-gen.cpp ^
   /link /LIBPATH:..\..\third_party\ffmpeg\x64\release\lib avcodec.lib avutil.lib
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" ( echo BUILD FAILED & exit /b %RC% )
echo Built: %~dp0ds4-tone-gen.exe
