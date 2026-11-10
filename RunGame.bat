@echo off
setlocal
set "T54UEExe=%~dp0Saved\StagedBuilds\Windows\T54UE.exe"
if not exist "%T54UEExe%" (
    >&2 echo Missing "%T54UEExe%". Package the Windows build first. See README.md.
    exit /b 1
)
set "T54UEOptions="
if "%~1"=="" set "T54UEOptions=-T5NewGame -T5Quest=0003372B"
"%T54UEExe%" -T5PapyrusContinueOnMissingNative %T54UEOptions% %*
exit /b %errorlevel%
