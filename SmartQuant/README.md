# SmartQuant

SmartQuant 是一个基于 C++17 的静态链接量化分析库，提供黄金 ETF 518880 量化交易模型 V6.0 所需的核心信号与风控流程。

## 构建依赖

- CMake >= 3.20
- Ninja
- C++17 编译器
- vcpkg（当前版本无外部三方依赖）

## 构建步骤（Linux/macOS/Windows）

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

构建输出：
- Linux/macOS: `libsmartquant.a`
- Windows: `smartquant.lib`

## 公共 API

`include/SmartQuant.h` 提供：
- 引擎初始化/销毁
- 实时行情推送（触发信号计算）
- 当前交易信号查询
- Buy/Sell/SellAll 回调注册
- 告警回调注册
- 每日收盘风控触发
- 当前状态查询
