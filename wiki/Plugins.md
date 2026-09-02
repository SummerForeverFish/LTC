# 策略插件（运行时动态加载）

LTC 支持把策略编译成独立 DLL（或 Linux 的 so），在运行时由主程序 `LoadLibrary` 加载并注册，**换策略零重编主程序**。

## 为什么用纯 C 接口（ABI）

C++ 的 STL / 异常 / 名称修饰**没有稳定 ABI**，跨 DLL 边界传 `std::shared_ptr` / `std::string` 容易在不同编译器/CRT 下崩溃。约定：

- 字符串一律 `const char*`，由调用方持有；
- 策略对象由**插件侧 `new`、也由插件侧 `delete`**（宿主把 `ltc_plugin_destroy` 包进 `shared_ptr` 的自定义 deleter，保证「谁分配谁释放」）。

## 插件需导出的 C 函数（`include/ltc/core/plugin_abi.h`）

| 函数 | 作用 |
| --- | --- |
| `int ltc_plugin_api_version(void)` | 返回 `LTC_PLUGIN_API_VERSION`（宿主校验，不匹配拒绝加载） |
| `int ltc_plugin_strategy_count(void)` | 本插件导出的策略类型个数 |
| `const char* ltc_plugin_strategy_type(int i)` | 第 i 个类型名（`static` 字符串） |
| `const char* ltc_plugin_strategy_desc(int i)` | 第 i 个描述（可为 NULL） |
| `void* ltc_plugin_create(type, name, params)` | 创建实例，返回裸指针；失败返回 NULL |
| `void ltc_plugin_destroy(void* obj)` | 销毁由 create 返回的对象（必须与 create 配对） |

## 最小插件模板（`plugins/my_ma_plugin.cpp`）

```cpp
#define LTC_PLUGIN_BUILD          // 必须在 include plugin_abi.h 之前定义
#include "ltc/core/plugin_abi.h"
#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/strategy_registry.hpp"

using namespace ltc;

namespace {
class MaCrossStrategy : public BaseStrategy {
public:
    MaCrossStrategy(const std::string& name, int fast, int slow, double vol, bool live)
        : BaseStrategy(name), fast_(fast), slow_(slow), vol_(vol), live_(live) {}
    void on_tick(const TickData& tk) override { feed(tk.last_price, tk.vt_symbol); }
    void on_bar (const BarData& b)  override { feed(b.close, b.vt_symbol); }
    // ... 双均线金叉开多/死叉平多 ...
private:
    int fast_, slow_; double vol_; bool live_; /* + 持仓/缓冲 */
};
const char* kType = "MACross";
const char* kDesc = "双均线tick策略(最小模板) 参数: fast,slow,vol,live";
}

extern "C" {
LTC_PLUGIN_EXPORT int  ltc_plugin_api_version(void) { return LTC_PLUGIN_API_VERSION; }
LTC_PLUGIN_EXPORT int  ltc_plugin_strategy_count(void) { return 1; }
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_type(int i) { return i == 0 ? kType : nullptr; }
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_desc(int i) { return i == 0 ? kDesc : nullptr; }
LTC_PLUGIN_EXPORT void* ltc_plugin_create(const char* type, const char* name, const char* params) {
    if (!type || std::string(type) != kType) return nullptr;
    StrategyParams p = parse_params(params ? params : "");
    auto* s = new MaCrossStrategy(name ? name : kType,
        param_int(p, "fast", 30), param_int(p, "slow", 120),
        param_double(p, "vol", 1.0), param_bool(p, "live", false));
    return static_cast<void*>(s);
}
LTC_PLUGIN_EXPORT void ltc_plugin_destroy(void* obj) {
    delete static_cast<BaseStrategy*>(obj);
}
}
```

> `LTC_PLUGIN_BUILD` 让 `LTC_PLUGIN_EXPORT` 在插件侧展开为 `__declspec(dllexport)`（宿主侧为空）。MSVC 重复定义宏可能报 C4005，属无害警告。

## 编译

```powershell
cl /std:c++17 /utf-8 /EHsc /O2 /LD /DLTC_PLUGIN_BUILD /I include plugins/my_ma_plugin.cpp /Fe:plugins/my_ma_plugin.dll
```

## 加载与运行

**C++：**
```ini
[plugins]
myma = plugins/my_ma_plugin.dll
[strategy]
type = MACross
name = MA1
params = fast=30,slow=120,vol=1.0,live=1
```
或 `ltc list` 查看已加载插件。

**Python：**
```python
err = v.PluginLoader.load("plugins/my_ma_plugin.dll")  # 空串=成功
print(v.StrategyRegistry.list())                        # 含 MACross
st = v.StrategyRegistry.create("MACross", "M", {"fast":"30","vol":"1.0","live":"1"})
```

`PluginLoader` 持锁校验 ABI 版本、枚举并注册策略类型，并把 `ltc_plugin_destroy` 包进 `shared_ptr` deleter；`unload_all()` 必须在所有由插件创建的策略实例释放后再调用，否则析构会跳进已卸载代码段而崩溃。

## 完整范例

- `plugins/demo_plugin.cpp`（`MomentumTick` 动量 tick 策略）
- `plugins/bollinger_plugin.cpp`（`BollingerPlugin`，含 `BarGenerator` 把 tick 聚合成分钟 K 后做均值回归）
- `plugins/my_ma_plugin.cpp`（最小模板，教学用）
