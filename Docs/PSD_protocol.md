# PSD 协议增量

MCA2/MCF2 版本仍为 2，保留普通事件 type 1 的 12 字节格式。新增模式 5、type 7、能力位 `0x00000400`。设备为 XC7A50TFTG256-2，采样率 125 MS/s。

## 配置

| Target | 地址 | 含义 |
|---|---|---|
| Global | 0x0000 | 采集模式：5=PSD，仅 UDP |
| Global | 0x010A | 格式能力，bit 10=PSD |
| Global | 0x0111 | PSD 最大总窗口，10000 ns |
| Channel 0 | 0x0014 | 总窗口 N，样本数 |
| Channel 0 | 0x0015 | 前置窗口 P，样本数 |
| Channel 0 | 0x001B | 尾门起点 S，相对触发的有符号样本数 |
| Channel 0 | 0x001C | 尾门终点 E，相对触发的有符号样本数 |

S/E 用 32 位二进制补码符号扩展。例如 −4 写入 0xFFFFFFFC。端点必须是合法整数，不接受把微秒浮点值直接写入寄存器。

写入 shadow 后 COMMIT 原子应用。PSD 校验 `6≤N≤1250`、`P<N`、`−P≤S<E≤N−P`、峰值插值关闭；不合法返回 VALIDATION_FAILED，活动配置不改变。PSD 运行时拒绝窗口写入，停止后修改。尾门独立结束，不要求 E=N−P。现有普通模式总窗口仍可到 2500 点。

## Type 7 记录

MCA2 共用头 20 字节不变，record_count×24 必须与 payload_bytes 相符；一包不超过 8 条，部分批次超时 100 μs。多字节字段均为网络字节序。

| 字节偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 6 | 48 位触发时间戳，8 ns/tick |
| 6 | 2 | 原峰值 Q14.2 |
| 8 | 3 | 原绝对值积分，24 位饱和值 |
| 11 | 1 | 原 flags：通道 bits 7:3、pileup bit 2、overflow bit 1、负极性 bit 0 |
| 12 | 4 | Qtotal，有符号 32 位 |
| 16 | 4 | Qtail，有符号 32 位 |
| 20 | 2 | 锁存基线，低 14 位有效 |
| 22 | 1 | quality：bit 0=窗口内 ADC overrange，bit 1=电荷运算异常 |
| 23 | 1 | 保留，必须为 0 |

quality 的其他位为 0。Host 拒绝非零保留位和超出 14 位的基线。PSD 有效性不能只依据旧 flags 的 overflow 位，因为原绝对值积分可能单独饱和。

内部 FIFO 232 位由 `config_version[31:0] | type[7:0] | payload[191:0]` 组成。普通事件的 payload 低 96 位为原事件，高 96 位为零；PSD payload 按上表排列。配置版本与类型在事件开始时锁存；发送器不混装不同类型或版本，已弹出但不属于本批的记录保留到下一包。

## 原始文件

`MCA_PSD_TXT_V1` 是 UTF-8 TAB 文件。文件头保存 125 MHz 采样率、8 ns 步长、N/P/S/E、滤波开关、阈值与极性。列依次为：

```
timestamp config_version peak_raw integral_raw baseline qtotal qtail channel pileup overflow polarity quality
```

时间戳和峰值保持原整数表示；电荷是带符号十进制值；通道编号保持协议的从 0 开始约定。分类阈值与结果另行导出，原始文件不保存随调试改变的候选类别。
