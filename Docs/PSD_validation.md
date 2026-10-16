# PSD 源码交付状态

本文保留源工作区的历史验证摘要。大型生成报告、检查点、失败 bitstream 和交付二进制不纳入源码仓库；下方路径用于说明原始记录位置，不表示克隆仓库中包含这些产物。

## 本次交付

PSD 功能已实现。最后一次修改将窗口合法性检查拆为两级寄存器：第一级计算窗口边界并锁存同一份 shadow 参数，第二级比较边界并寄存校验结果。控制应答只读取校验结果，移除原来的“窗口减法→比较→应答使能”组合链。

已运行修改后的控制、整机、PSD 整数参考和混合事件 UDP 仿真，全部通过。控制测试另覆盖连续配置事务、非法尾门恢复为合法尾门后立即 COMMIT，以及 COMMIT 必须等待采样域确认。

此前全量 RTL 回归 23 项通过；Host 三组测试全部通过：PSD 10 项、core 54 项、UI 15 项（含各组初始化/清理）。Host 已生成 `Host/build-psd/mca_studio.exe`。百万条缓存快照的单次测试耗时约 5 ms，此数值不是硬件满速保证。

**按用户要求，最后的合法性检查流水线修改后不再运行综合实现。新版本的资源、setup/hold、CDC 和 bitstream 由用户重新生成验证，当前不能标为时序验收完成。**

## 历史实现报告

`HDL/reports/psd` 的现有报告对应最后修改之前的实现：WNS −0.027 ns、TNS −0.043 ns、5 个 setup 失败端点；hold slack +0.028 ns。违例位于控制域窗口检查至应答使能，而不是 PSD 采样积分链。进入 PSD 模块的最差 setup slack 为 +1.538 ns。

增加运行状态的两级同步后，CDC-1 告警计数由原工程 1214 降至 1073。新增 PSD 边界使用 shadow 总线保持和配置 toggle/ack 握手，工具以 CDC-15 告警显示；剩余既有 CDC-1/10/13 项未在本次全部重构。报告不是“CDC 零告警”证明。

历史 bitstream 已归档为 `HDL/reports/psd/pre_validation_pipeline_timing_failed.bit`，不作为本次交付的可下载版本。当前 `HDL/delivery` 保存源码 SHA256 清单及需要重建的说明。

## 用户执行

在源码仓库根目录执行：

```powershell
.\HDL\build_psd.ps1
```

此入口使用 12 个并行 jobs、50T 器件，并重新生成 IP。它会在检查 setup/hold 均无负 slack 后，才复制新 bitstream 到 `HDL/delivery/single_channel_mca_psd.bit`。也可直接在新的 ASCII 工程路径运行：

```powershell
& "$env:VIVADO_BIN\vivado.bat" -mode batch -source HDL\scripts\build_psd.tcl
```

之后核对 `HDL/reports/psd/timing_impl.rpt`、`psd_paths.rpt`、`cdc_impl.rpt` 和 `utilization_impl.rpt`；受约束 setup/hold 应无违例、TNS=0，新增 PSD 路径以 ≥0.3 ns setup slack 为目标。未新增放宽时钟、multicycle 或 false-path 来掩盖失败。

原有 ADC/PHY I/O 延时约束不完整，上板及真实探测器 FOM 仍需实测。
