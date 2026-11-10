@echo off
call "%~dp0RunGame.bat" -T5SelectSave -T5RaceMenu %*
exit /b %errorlevel%
