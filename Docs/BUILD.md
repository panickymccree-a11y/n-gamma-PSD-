# 构建说明

## Host 上位机

依赖 Windows 10/11、Visual Studio 2022 C++ 工具和 Windows SDK、Qt 6.8 的 MSVC 2022 版本、CMake 3.21 或更新版本，以及 Ninja。

1. 复制 `Host/local_env.example.cmd` 为 `Host/local_env.cmd`，填写 `QT_ROOT`、`CMAKE_BIN` 和 `NINJA_BIN`。该本机配置文件已加入 Git 忽略规则。
2. 从 x64 Visual Studio Developer Prompt 运行 `Host/build_and_test.cmd`。也可以在本机配置文件中填写 `VSDEVCMD`，由脚本加载编译环境。
3. 可选的持续吞吐脚本为 `Host/run_throughput.ps1`。它读取 `QT_ROOT`，并可选读取 `VC_RUNTIME`。

构建产物放在 `Host/build-psd`，无需提交到版本库。Qt 应包含 Widgets、Network、SerialPort、Charts、Concurrent 和 Test 组件。

## HDL FPGA

依赖 Vivado 2020.2 和 Artix-7 器件支持。Vivado 工程目标为 `xc7a50tftg256-2`。

- 将 `VIVADO_BIN` 设置为 Vivado `bin` 目录后，运行 `HDL/sim/run_xsim.cmd` 执行 XSim 回归。
- 运行 `HDL/build_psd.ps1` 进行综合实现。可传入 `-Vivado <vivado.bat路径>` 和 `-BuildRoot <ASCII构建目录>`；默认构建目录位于系统临时目录。Vivado 2020.2 对非 ASCII 构建路径有限制。
- 也可在 Vivado Tcl 环境中运行 `HDL/scripts/create_project.tcl` 重建工程源文件集；该脚本只创建工程，不自动综合或生成 bitstream。

仓库保留 RTL、约束、测试平台、参考向量和 IP 的 `.xci` 配置。Vivado 生成的库文件、网表、缓存、报告及 bitstream 均在本机重建，不纳入 Git。

## MATLAB 合成脉冲演示

在 MATLAB 中运行 `demo_ngamma_psd.m`。脚本不依赖额外工具箱；输出写入 `demo_output/`，该目录已忽略。参数来源、假设和模型适用范围见 `Docs/PSD_synthetic_demo.md`。

## 验证记录边界

`Docs/throughput_delivery_20261013.md` 和 `Docs/PSD_validation.md` 保存历史软件与 RTL 验证记录。它们不代表硬件实测、当前 bitstream 时序验收或上板验证；克隆后如需确认当前源码状态，应在目标工具链中重新运行相应流程。
