// plugin_abi.h - 策略插件的 C 接口约定（宿主程序与插件 DLL 共享）
//
// 为什么用纯 C 接口：C++ 的 STL/异常/名称修饰没有稳定 ABI，跨 DLL 边界传 std::shared_ptr
// 或 std::string 容易在不同编译器/CRT 下崩。这里约定：
//   - 策略对象由插件侧 new，也由插件侧 delete（destroy 回调）
//   - 字符串一律 const char*，由调用方持有
// 宿主拿到裸指针后包成 shared_ptr 并挂自定义 deleter 调 ltc_plugin_destroy，
// 从而保证「谁分配谁释放」。
#ifndef LTC_PLUGIN_ABI_H
#define LTC_PLUGIN_ABI_H

#define LTC_PLUGIN_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/* 插件编译时定义 LTC_PLUGIN_BUILD 才会 dllexport；宿主侧只用到函数指针类型，不导出 */
#if defined(_WIN32)
#  if defined(LTC_PLUGIN_BUILD)
#    define LTC_PLUGIN_EXPORT __declspec(dllexport)
#  else
#    define LTC_PLUGIN_EXPORT
#  endif
#elif defined(__GNUC__)
#  define LTC_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#  define LTC_PLUGIN_EXPORT
#endif

/* ABI 版本号，宿主加载时校验，不匹配则拒绝加载 */
LTC_PLUGIN_EXPORT int ltc_plugin_api_version(void);

/* 插件内含有的策略类型个数 */
LTC_PLUGIN_EXPORT int ltc_plugin_strategy_count(void);

/* 第 i 个策略的类型名（返回静态字符串，插件持有其生命周期） */
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_type(int index);

/* 第 i 个策略的描述（可为 NULL） */
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_desc(int index);

/* 创建策略：type=类型名, name=实例名, params="k=v,k=v"；失败返回 NULL */
LTC_PLUGIN_EXPORT void* ltc_plugin_create(const char* type, const char* name,
                                              const char* params);

/* 销毁由 ltc_plugin_create 返回的对象（必须与 create 配对） */
LTC_PLUGIN_EXPORT void ltc_plugin_destroy(void* obj);

/* 函数指针类型，供宿主 GetProcAddress 后转换使用 */
typedef int         (*ltc_api_version_fn)(void);
typedef int         (*ltc_count_fn)(void);
typedef const char* (*ltc_type_fn)(int);
typedef const char* (*ltc_desc_fn)(int);
typedef void*       (*ltc_create_fn)(const char*, const char*, const char*);
typedef void        (*ltc_destroy_fn)(void*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LTC_PLUGIN_ABI_H */
