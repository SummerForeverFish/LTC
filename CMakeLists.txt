cmake_minimum_required(VERSION 3.15)
project(ltc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W3 /O2)
else()
    add_compile_options(-O2 -Wall)
endif()

# 头文件目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})   # 让 strategies 子目录可被 #include "strategies/..."

# 主程序（回测 + 实盘演示，零外部依赖）
add_executable(ltc
    main.cpp
)

# 实盘(CTP)接入：指定 CTP API 目录后链接行情/交易库
#   cmake -B build -DLTC_CTP_DIR="D:/.../CTP_V6.7.11_P4_20251125"
# 不指定时 main.cpp 仍会 #include ctp_gateway.hpp（依赖 ThostFtdc 头），编译会失败；
# 纯回测/币安Stub 演示推荐改用 build.ps1（已内置 CTP 路径）。
option(LTC_CTP_DIR "" OFF)
if (LTC_CTP_DIR)
    target_include_directories(ltc PRIVATE ${LTC_CTP_DIR})
    target_link_libraries(ltc PRIVATE
        ${LTC_CTP_DIR}/win64/thostmduserapi_se.lib
        ${LTC_CTP_DIR}/win64/thosttraderapi_se.lib)
endif()

# 如需接真实交易所，链接 libcurl 与 ixwebsocket/websocketpp，并在
# binance_gateway.hpp 中提供对应 HttpClient / WsClient 实现后注入 BinanceGateway。
# find_package(CURL REQUIRED)
# target_link_libraries(ltc PRIVATE CURL::libcurl)

# ---------------------------------------------------------------------------
# Python 绑定 (nanobind)：让 Python 直接回测 / 实盘
# 构建（需先 pip install nanobind）：
#   cmake -B build -DLTC_BUILD_PYTHON=ON \
#     -DCMAKE_PREFIX_PATH="$(python -c "import nanobind,os;print(os.path.dirname(nanobind.cmake_dir()))")"
#   cmake --build build
# 生成的 ltc.pyd 与 run_backtest.py / run_live.py 放在同级目录即可 import。
# 注：本沙箱无 cmd.exe 且 D: 盘 SDK 不在默认注册表，故用 build_py.ps1 直接调 cl.exe 构建。
# ---------------------------------------------------------------------------
option(LTC_BUILD_PYTHON "Build nanobind Python module" OFF)
if (LTC_BUILD_PYTHON)
    find_package(Python 3.12 COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(nanobind CONFIG REQUIRED)
    nanobind_add_module(ltc bindings/ltc.cpp)
    target_include_directories(ltc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if (MSVC)
        target_compile_options(ltc PRIVATE /utf-8)
    endif()
endif()
