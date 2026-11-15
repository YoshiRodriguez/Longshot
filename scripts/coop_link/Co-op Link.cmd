@echo off
rem Longshot Co-op Link: co-op over the internet with no port forwarding (README.md beside this file)
cd /d "%~dp0"
py -3 -c "import aiortc" 2>nul || py -3 -m pip install -r requirements.txt
start "" pyw -3 coop_link.py
