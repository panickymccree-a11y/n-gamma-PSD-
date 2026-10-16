# single_channel_mca

单通道 AD9246 MCA 的 Vivado 2020.2 FPGA工程与RTL实现。Qt上位机位于
同级目录 `../Host`。

入口文件：

- `single_channel_mca.xpr`：工程清单；
- `scripts/create_project.tcl`：可重复重建工程；
- `scripts/run_vivado.tcl`：仿真、综合、资源、时序和 CDC 报告；
- `constraints/fpga_user_io.xdc`：来自 `FPGA用户IO及电平标准.xlsx` 的 LOC/IOSTANDARD；
- `../Docs/PSD_protocol.md`：PSD 的 MCA2/MCF2 协议扩展；
- `../Docs/PSD_validation.md`：源码交付与历史验证边界；
- `../Host/build_and_test.cmd`：上位机构建与测试入口。

功能范围：

- 能谱模式：14 位单通道事件采集、峰值/积分能谱、事件 TXT、ROI 与拟合；
- 波形模式：与能谱互斥的 12500 点（8 ns 间隔，−30 μs～+70 μs）示波器式
  显示，支持连续/单次触发、缩放、overrange 标记和 CSV 导出；波形采集
  与能谱共享阈值、极性和基线配置，极性现在真正参与两种模式的判别。

本阶段不依赖旧工程源文件，也不依赖旧工程生成结果。没有安装 Vivado 的环境
只能进行文件级检查；最终综合、CDC、时序和 XSim 结果需在本机 Vivado 2020.2
执行脚本获得。

构建目录、报告、网表、缓存及 bitstream 不纳入源码仓库。IP 的 `.xci` 配置保留，Vivado 2020.2 会在本机重新生成实现文件。工具环境设置见 `../Docs/BUILD.md`。
