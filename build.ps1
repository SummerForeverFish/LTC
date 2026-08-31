# build.ps1 - 用 MSVC (cl.exe) 直接编译 LTC 主程序与策略插件 DLL
#
# 职责：
#   1) 编译 build/ltc.exe —— 含 main.cpp + strategies/register_builtin.cpp
#      （内置策略注册表）+ src/event_engine.cpp（事件引擎实现），并链接 CTP 实盘库。
#   2) 编译 plugins/demo_plugin.dll —— 独立策略插件，运行时按 [plugins] 配置加载，
#      无需重编主程序。
#   3) 拷贝 CTP 运行期 dll（thost*_se.dll）到 build/ 旁。
# 前置：已安装 Visual Studio 2022(MSVC) 与 Windows SDK；本机 CTP API 路径见 $ctp。
# 用法：在 vncpp 目录下执行  powershell -File build.ps1
#
# 关键编译选项说明：
#   /DWIN32_LEAN_AND_MEAN /DNOMINMAX
#     —— 缩小 windows.h 体积并禁用其定义的 min/max/ERROR 等宏，
#        避免污染 C++ 标准库的 std::min/std::max 及框架内的 ERROR 枚举。
#   /utf-8        —— 源码与执行字符集统一为 UTF-8，保证中文日志不乱码。
#   /Fo:build\    —— 目标文件(*.obj)统一输出到 build/（用正斜杠写法，cl 兼容）。
#   /I .          —— 将项目根加入包含路径，使 register_builtin.cpp 能用
#                    "strategies/xxx.hpp" 形式从根目录解析。
#   CTP 头/lib    —— 经 $env:INCLUDE / $env:LIB 追加 thostmduserapi_se 头与 win64 lib。
#   $PSScriptRoot —— 以脚本所在目录作为项目根，目录改名/移动后无需改动即可编译。
# 项目根由脚本自身位置推导，目录改名后无需改这里
$wd = $PSScriptRoot
Start-Transcript -Path "$wd\build.log" -Force

$vs     = "D:\Program Files\Microsoft Visual Studio\18\Community"
$msvc   = "$vs\VC\Tools\MSVC\14.51.36231"
$sdk    = "D:\Windows Kits\10"
$sdkver = "10.0.26100.0"
$bin    = "$msvc\bin\Hostx64\x64"

$env:PATH    = "$bin;" + $env:PATH
$env:INCLUDE = "$msvc\include;$sdk\Include\$sdkver\ucrt;$sdk\Include\$sdkver\um;$sdk\Include\$sdkver\shared"
$env:LIB     = "$msvc\lib\x64;$sdk\Lib\$sdkver\ucrt\x64;$sdk\Lib\$sdkver\um\x64"

Set-Location $wd
if (!(Test-Path build))   { New-Item -ItemType Directory -Path build   | Out-Null }
if (!(Test-Path plugins)) { New-Item -ItemType Directory -Path plugins | Out-Null }

# CTP 实盘 API 头文件与库 (lightning-futures-master 提供的 V6.7.11 P4)
$ctp = "D:\files\app\lightning-futures-master\lf\api\CTP_V6.7.11_P4_20251125"
$env:INCLUDE += ";$ctp"
$env:LIB     += ";$ctp\win64"

Write-Host "=== INCLUDE ===`n$($env:INCLUDE)"
Write-Host "=== LIB ===`n$($env:LIB)"

# ---- 1) 主程序 ------------------------------------------------------------
# main.cpp 不再包含任何策略代码；内置策略由 register_builtin.cpp 注册进策略表。
# event_engine.cpp 是 EventEngine 的方法实现（独立 TU，需 BaseStrategy 完整类型）。
# /I . 让 register_builtin.cpp 里的 "strategies/xxx.hpp" 从项目根开始解析。
Write-Host "=== compiling main ==="
& "$bin\cl.exe" /nologo /std:c++17 /utf-8 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I . /I include /I strategies `
    /Fe:build\ltc.exe /Fo:build\ main.cpp strategies\register_builtin.cpp src\event_engine.cpp `
    "$ctp\win64\thostmduserapi_se.lib" "$ctp\win64\thosttraderapi_se.lib"
Write-Host "main EXIT=$LASTEXITCODE"

# ---- 2) 外部策略插件（独立编译成 DLL，运行时按 [plugins] 配置加载，无需重编主程序）----
Write-Host "=== compiling demo plugin ==="
& "$bin\cl.exe" /nologo /std:c++17 /utf-8 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /LD /DLTC_PLUGIN_BUILD /I . /I include `
    /Fe:plugins\demo_plugin.dll /Fo:build\ plugins\demo_plugin.cpp
Write-Host "demo plugin EXIT=$LASTEXITCODE"

Write-Host "=== compiling bollinger plugin ==="
& "$bin\cl.exe" /nologo /std:c++17 /utf-8 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /LD /DLTC_PLUGIN_BUILD /I . /I include `
    /Fe:plugins\bollinger_plugin.dll /Fo:build\ plugins\bollinger_plugin.cpp
Write-Host "bollinger plugin EXIT=$LASTEXITCODE"

Write-Host "EXIT_CODE=$LASTEXITCODE"

# 把运行期需要的 dll 拷到 exe 旁
Copy-Item "$ctp\win64\thostmduserapi_se.dll" build\ -Force
Copy-Item "$ctp\win64\thosttraderapi_se.dll" build\ -Force

Stop-Transcript
exit $LASTEXITCODE
