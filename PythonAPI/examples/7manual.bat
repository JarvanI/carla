@echo off
setlocal

set "first_command1=python showflag_rendering.py"
set "first_command2=python manual_control.py"
set "other_command=python manual_control.py"
set "count=7"
set "delay=3"

:: 运行第一个 CMD 窗口并执行两个命令
start cmd /k "%first_command1% & %first_command2%"
timeout /t %delay% /nobreak >nul

:: 运行其余 CMD 窗口
for /L %%i in (2,1,%count%) do (
    start cmd /k "%other_command%"
    timeout /t %delay% /nobreak >nul
)

endlocal