@echo off
setlocal
chcp 65001 >nul

where g++ >nul 2>nul
if errorlevel 1 (
    echo 未找到 g++，请先安装 MinGW-w64 并加入 PATH。
    exit /b 1
)

if not exist bin mkdir bin

g++ -std=c++17 -O2 -mwindows -static -o bin\CodeTool.exe CodeTool.cpp -lcomdlg32 -lgdi32
if errorlevel 1 (
    echo 编译失败。
    exit /b 1
)

echo 已生成 bin\CodeTool.exe
endlocal
