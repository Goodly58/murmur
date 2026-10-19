@echo off
rem Serves Murmur on http://127.0.0.1:8742 and opens it. Close this window to stop the server.
rem (The installed app and the GitHub Pages copy need no server; this is the offline-from-folder route.)
title Murmur (close this window to stop)
cd /d "%~dp0"
start "" cmd /c "ping -n 2 127.0.0.1 >nul & start http://127.0.0.1:8742/"
py -3.13 serve.py 8742
