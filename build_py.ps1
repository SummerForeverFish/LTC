# build_py.ps1 - Build ltc.pyd (nanobind Python extension) with cl.exe directly.
#
# Bypasses CMake's Windows SDK auto-detection on D: drive.
# Compile units: nb_combined.obj (nanobind static lib) + ltc.obj (bindings/ltc.cpp)
#                + register_builtin.obj (builtin strategies) + event_engine.obj (event engine).
# Link libs: Python313.lib + CTP libs (thostmduserapi_se / thosttraderapi_se).
# Key options:
#   /std:c++20        - required by nanobind
#   /DNB_STATIC       - link nanobind statically (no runtime nb dll dependency)
#   /DWIN32_LEAN_AND_MEAN /DNOMINMAX - prevent windows.h macro pollution
#   /utf-8            - source/exec charset UTF-8
# NOTE: keep this file ASCII-only. PowerShell 5.1 reads BOM-less .ps1 as ANSI(GBK);
#       UTF-8 Chinese comments get misread and can silently break parsing.
# Prereq: MSVC + Windows SDK installed; Python 3.13 & nanobind paths below; CTP path below.
# Usage: run in vncpp dir ->  powershell -File build_py.ps1
$ErrorActionPreference = "Stop"
# Project root derived from script location; fall back to cwd when $PSScriptRoot is empty.
$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$log  = "$root\build_py.log"
"=== build_py start $(Get-Date) ===" | Out-File -FilePath $log -Encoding ascii

$msvc = "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$sdk  = "D:\Windows Kits\10"
$ver  = "10.0.26100.0"
$bin  = "$msvc\bin\Hostx64\x64"
$py   = "C:\Users\admin\.workbuddy\binaries\python\versions\3.13.12"
$nb   = "$py\Lib\site-packages\nanobind"
$ctp  = "D:\files\app\lightning-futures-master\lf\api\CTP_V6.7.11_P4_20251125"

$env:PATH   = "$bin;" + $env:PATH
$env:INCLUDE = "$msvc\include;$sdk\Include\$ver\ucrt;$sdk\Include\$ver\um;$sdk\Include\$ver\shared;$py\include;$nb\include;$nb\ext\robin_map\include;$root\include;$root;$ctp"
$env:LIB     = "$msvc\lib\x64;$sdk\Lib\$ver\ucrt\x64;$sdk\Lib\$ver\um\x64;$py\libs;$ctp\win64"

$build = "$root\build"
if (!(Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

function Run($exe, $argstr) {
    "[CMD] $exe $argstr" | Out-File -FilePath $log -Encoding ascii -Append
    # NOTE: param must NOT be named $args/$argv (PowerShell automatic variables).
    # Paths in this project contain no spaces, so strip quotes then split on spaces.
    $toks = @($argstr.Replace('"', '').Split(' ') | Where-Object { $_ -ne '' })
    # Compiler warnings go to stderr; with EAP=Stop the first 2>&1 ErrorRecord
    # aborts the pipeline, so relax EAP while the tool runs.
    $eapBak = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe $toks 2>&1 | Out-File -FilePath $log -Encoding ascii -Append
    $ErrorActionPreference = $eapBak
    "[EXIT] $LASTEXITCODE" | Out-File -FilePath $log -Encoding ascii -Append
    if ($LASTEXITCODE -ne 0) {
        "FAILED ($LASTEXITCODE) see $log" | Out-File -FilePath $log -Encoding ascii -Append
        throw "build step failed"
    }
}

$std = "/nologo /std:c++20 /utf-8 /EHsc /O2 /DNB_STATIC /DWIN32_LEAN_AND_MEAN /DNOMINMAX /c"

# 1) nanobind static lib (combined source)
Run "$bin\cl.exe" "$std `"$nb\src\nb_combined.cpp`" /Fo:`"$build\nb_combined.obj`""

# 2) bindings
Run "$bin\cl.exe" "$std `"$root\bindings\ltc.cpp`" /Fo:`"$build\ltc.obj`""

# 2b) builtin strategy registration unit
Run "$bin\cl.exe" "$std `"$root\strategies\register_builtin.cpp`" /I `"$root`" /I `"$root\include`" /Fo:`"$build\register_builtin.obj`""

# 2c) EventEngine implementation (lock-free MPMC / zero-copy)
Run "$bin\cl.exe" "$std `"$root\src\event_engine.cpp`" /I `"$root\include`" /Fo:`"$build\event_engine.obj`""

# 3) link into .pyd (with CTP live libs)
Run "$bin\link.exe" "/nologo /DLL /EXPORT:PyInit_ltc /OUT:`"$root\ltc.pyd`" `"$build\nb_combined.obj`" `"$build\ltc.obj`" `"$build\register_builtin.obj`" `"$build\event_engine.obj`" python313.lib thostmduserapi_se.lib thosttraderapi_se.lib"

"=== build_py OK ===" | Out-File -FilePath $log -Encoding ascii -Append
