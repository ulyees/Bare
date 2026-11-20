@echo off
chcp 65001 >nul 2>&1
setlocal
echo 正在编译 Bare ...

where windres >nul 2>&1
if errorlevel 1 (
  echo [错误] 未找到 MinGW-w64 的 windres，请把 MinGW-w64 的 bin 目录加入 PATH。
  pause
  exit /b 1
)
windres -i app.rc -o app_res.o -O coff
if errorlevel 1 (
  echo [错误] 资源编译失败
  pause
  exit /b 1
)
g++ -std=c++17 -O2 -s -mwindows -static Bare.cpp app_res.o -o Bare.exe -lcomdlg32 -ldwmapi -lshell32 -ladvapi32 -lgdi32 -luser32 -lole32 -luuid
if errorlevel 1 (
  echo [错误] 编译失败
  pause
  exit /b 1
)
del app_res.o >nul 2>&1
echo 完成：Bare.exe （可随意改名）
pause
