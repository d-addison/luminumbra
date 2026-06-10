@echo off
rem Rewrites the retired gpt-5.3-codex model id (hardcoded by the dispatch
rem spawner) to gpt-5.5, which ChatGPT-account Codex CLI supports.
setlocal enabledelayedexpansion
set "ARGS=%*"
if defined ARGS set "ARGS=!ARGS:gpt-5.3-codex=gpt-5.5!"
call "%APPDATA%\npm\codex.cmd" !ARGS!
exit /b %ERRORLEVEL%
