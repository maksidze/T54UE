@echo off
call "%~dp0RunGame.bat" -T5LoadGame %*
exit /b %errorlevel%
