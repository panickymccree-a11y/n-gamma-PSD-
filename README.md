# MCA 与 PSD 探测系统

本仓库包含单通道多道分析仪（MCA）的 FPGA/上位机源码，以及用于尾部电荷比 PSD 调试的功能。另含基于 EJ-276D 参数的合成脉冲演示；演示数据是模型生成，不是探测器实测数据。

## 目录

- `HDL/`：Vivado 2020.2 工程、Artix-7 FPGA RTL、约束、IP 配置和 XSim 回归。
- `Host/`：Qt 6 / C++20 Windows 上位机、协议处理、能谱/波形/PSD 页面及软件测试。
- `Docs/`：架构、协议、PSD 模型、构建步骤和历史验证记录。
- `test_vectors/`：PSD RTL 回归使用的参考输入与期望值。
- `demo_ngamma_psd.m`：可参数化的合成 n/γ PSD 演示脚本。

## 构建

Host 依赖 Windows、Visual Studio 2022 C++ 工具、Qt 6.8 MSVC 2022 版本、CMake 和 Ninja。先按 [`Docs/BUILD.md`](Docs/BUILD.md) 设置本机工具路径，再运行 `Host/build_and_test.cmd`。

HDL 工程使用 Vivado 2020.2 和器件 `xc7a50tftg256-2`。在 Vivado 环境中设置 `VIVADO_BIN` 后，可运行 `HDL/sim/run_xsim.cmd` 做仿真；综合实现入口和参数见 `HDL/README.md` 与 `Docs/BUILD.md`。

## 说明

Git 忽略规则排除了本机偏好、构建目录、安装包、仿真缓存、综合报告和 bitstream。源代码、Vivado IP 配置、仿真用例及参考向量保留在仓库中。硬件构建产物需要在目标环境重新生成。

软件/RTL 验证记录及尚未完成的综合时序和上板验证边界，见 [`Docs/throughput_delivery_20261013.md`](Docs/throughput_delivery_20261013.md) 和 [`Docs/PSD_validation.md`](Docs/PSD_validation.md)。这些记录描述对应日期的源码状态，不代表当前机器或目标硬件已完成验收。

本仓库尚未附带开源许可证；私有仓库访问权限由 GitHub 仓库设置控制。

## 子项目说明

- [上位机使用与构建](Host/README.md)
- [FPGA 工程与 IP 说明](HDL/README.md)
- [尾部电荷比 PSD 调试说明](PSD_README.md)
- [合成 n/γ 脉冲演示模型](Docs/PSD_synthetic_demo.md)
