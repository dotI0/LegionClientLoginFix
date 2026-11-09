@echo off
setlocal
if not "%~1"=="" if /I not "%~1"=="--all" (
    echo Usage: build.cmd [--all]
    exit /b 1
)
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo Open the x64 Native Tools Command Prompt for Visual Studio first.
    exit /b 1
)
where rc.exe >nul 2>&1
if errorlevel 1 (
    echo The Windows SDK resource compiler rc.exe is required.
    exit /b 1
)
set "SRC=%~dp0"
for %%D in ("%SRC%..") do set "PACKAGE=%%~fD"
set "WORK=%TEMP%\WoWLoginFix-26972-%RANDOM%-%RANDOM%"
mkdir "%WORK%" || exit /b 1
pushd "%SRC%" || exit /b 1
rc.exe /nologo /fo "%WORK%\Launcher.res" Launcher.rc
set "RC_STATUS=%ERRORLEVEL%"
popd
if not "%RC_STATUS%"=="0" exit /b 1
pushd "%WORK%" || exit /b 1
cl.exe /nologo /std:c++17 /W4 /EHsc /O2 /MT "%SRC%Loader.cpp" "%SRC%Cache.cpp" comdlg32.lib user32.lib /link /OUT:StartWoWWithLoginFix.exe Launcher.res
if errorlevel 1 goto failed
cl.exe /nologo /std:c++17 /W4 /EHsc /O2 /MT /DWOW_LOGIN_REPAIR /LD "%SRC%WowLoginProbe.cpp" "%SRC%WatcherRepair.cpp" /link /OUT:WowLoginRepair.dll
if errorlevel 1 goto failed
copy /Y "WowLoginRepair.dll" "%PACKAGE%\WowLoginRepair.dll" >nul || goto failed
copy /Y "StartWoWWithLoginFix.exe" "%PACKAGE%\StartWoWWithLoginFix.exe" >nul || goto failed

popd
exit /b 0

:failed
popd
exit /b 1
