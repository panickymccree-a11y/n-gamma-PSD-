# 吞吐优化第一版交付（2026-10-13）

本文是历史源码交付记录。部署包、构建日志、测试输出和生成的 FPGA 产物均为本机文件，不纳入源码仓库。

## 交付文件

- 上位机：`Host/delivery/MCAStudio_throughput/mca_studio.exe`，与 Qt 运行库、插件、D3D 编译库和 VC 运行库安装程序一起交付。
- HDL：当前 `HDL/single_channel_mca.xpr` 与 `HDL/rtl` 已更新；`HDL/delivery/rtl_sha256.txt` 标记交付源码。
- 根据用户最后指示，综合实现交由用户；本次已启动的实现任务已停止，没有生成或交付新 bitstream。历史报告和历史失败 bitstream 不代表这次源码结果。

## 实现内容

1. HDL：顶层能谱最大批次 64，PSD 有效上限 40，载荷分别为 788/980 字节；保留 100 μs 超时、64 条普通 FIFO 启动水位、pending 事件立即启动和类型/配置版本拆包。增加参数容量校验，保留 FIFO、仲裁和采集算法。
2. Host：绑定后设置并读取 8 MiB 接收缓冲；正式启动清零包统计时保留该诊断值。原调用顺序的独立 Qt 测试读取到实际 65536 字节，绑定后请求读取到 8388608 字节。
3. 序号检查使用 1024 槽环形窗口；顺序包常数开销，保留窗口内补齐与重复统计，超窗迟到单独统计并拒绝累计。默认关闭 typedEventsReady；显式启用后以 50 ms/4096 条批次通知，停止交付尾批。
4. 能谱和状态交付合并为最新快照；隐藏谱图不重画，显示时恢复当前数据。普通发布改用 50 ms 精确定时器。
5. PSD 保留最近百万条事件，新增/淘汰时增减直方图与质量计数；设置变更时重建，revision 过滤过时结果。工作线程生成密度图，100 ms 精确定时器发布；FOM 最多每秒计算一次并显示拟合事件版本。
6. PSD 输入直接接入线程安全会话，不跨线程调用 QWidget。TXT 编码/写盘独立线程、有界队列，满队列或写入错误明确报告；停止和析构排空已接受的记录。

线上 MCA2/MCF2 格式、积分、分类及 TXT 字段保持不变。新增接口和统计均在 Host 内部。

## 已运行的验证

- RTL：`HDL/sim/throughput_validation.log`，全量 24 项通过。新吞吐测试通过真实 udp_tx 请求流水核对每个 payload 字节，覆盖 64 条能谱、40 条 PSD、短包超时、混合类型/配置和 pending 尾条；原有控制、状态、波形及采集回归通过。
- Host 最终 Release：core 57、PSD 12、UI 15 项通过（包含初始化/清理）；`Host/throughput_build_final.log` 和 `Host/build-psd/*_tests_result.txt` 保存证据。新增序号回绕/乱序/重复/缺口/大跳号/迟到、增量统计独立参考及 TXT 停止尾批测试。
- 独立进程 UDP 发送器，经过真实 AcquisitionController 与 MainWindow 接收/显示；Qt offscreen 平台，固定合成事件，关闭 TXT 和 FOM。

| 模式 | 目标 cps | 条/包 | 时长 | 接收事件 | 丢包 / 解析错误 | 平均刷新 | 间隔 P95 | 停止响应 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 能谱 | 300,000 | 64 | 60 s | 18,000,000 | 0 / 0 | 20.00 Hz | 51.05 ms | 1.22 ms |
| 能谱 | 1,500,000 | 64 | 60 s | 90,000,000 | 0 / 0 | 20.00 Hz | 50.80 ms | 4.17 ms |
| PSD | 300,000 | 40 | 2 s | 600,000 | 0 / 0 | 10.00 Hz | 100.97 ms | 3.39 ms |

发送端实际速率达到各档目标，事件总数与对应直方图计数一致。软件测试是本机回环与 offscreen 主窗口结果，不是探测器/FPGA/物理网口上板实测，也不证明可在 PSD 长积分窗口下接受 1.5 Mcps 触发。

60 s 测试完成后仅修正正式启动清零包统计时的接收缓冲诊断值保留；最终 EXE 已重新通过三组回归及 PSD 短时检查。最终短时检查显示接收缓冲实际为 8388608 字节。

## 尚未验收

按“先交付一版”的指示，剩余长时矩阵已停止。PSD 60 s 持续吞吐、PSD 1.5 Mcps 合成流、旧 8 条小包的完整矩阵、FOM 开启时持续刷新、TXT 开启时吞吐、Windows 原生窗口持续刷新和上板结果尚未验收。修复前诊断结果保存在 `pre_bind_buffer_*`，不能混作最终版本通过记录。

RTL 已通过功能仿真；综合资源、setup/hold、CDC 与 bitstream 由用户执行验证，不标为完成。

## 使用与复测

先运行新 Host 与旧 HDL；随后使用更新后的 HDL 综合实现，比较新 Host＋新 HDL 的效果。上板分别记录 UDP 缺口、解析错误、FPGA FIFO 丢弃和采集拒绝，不混作同一种丢失。

Host 构建回归入口 `Host/build_and_test.cmd`；增量构建入口 `Host/build_throughput.cmd`。完整软件矩阵入口 `Host/run_throughput.ps1 -Seconds 60`，结果在 `Host/throughput_results`。
HDL 全量仿真入口 `HDL/sim/run_xsim.cmd`；用户综合实现入口 `HDL/build_psd.ps1`。当前 RTL 使用原有约束，未新增宽松时序例外。

## 校验

最终 EXE SHA256：`4948DEEE69121C7A8714E4D0B518A5859D442E24A25A4B2DC49F0F871AD2F7E0`。
`Host/source_sha256.txt`、`HDL/delivery/rtl_sha256.txt` 和包内 `payload_sha256.txt` 保存源码/载荷清单。程序未增加签名；本机测试不等同于干净机器安装验收。
