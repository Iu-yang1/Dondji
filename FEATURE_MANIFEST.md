# CN-RF 功能清单

本文记录 `CN_RF` 预设相对 Dondji/Fusion 的射频功能、来源、持久化位置和验证状态。实现保持 Dondji Motorola R7 UI、中文输入/字库、1024 个 MR 信道及 MDC1200；没有采用 IJV 的信道或菜单物理布局。

## 来源与实现边界

- Dondji 当前源码：UI、信道、扫描、MDC1200、USB/UART、频谱和发射安全状态机的基础。
- [IJV V4 模式规格](https://www.ijvradio.com/manual/pages/modes.html)、[带宽规格](https://www.ijvradio.com/manual/pages/bw.html)、[AGC/RF Gain 规格](https://www.ijvradio.com/manual/pages/agc.html)和[菜单规格](https://www.ijvradio.com/manual/pages/menu.html)：只作为公开行为规格。
- [BK4829 数据手册 DS-BK4829-E01 V1.0](https://device.report/m/b03fb6dba016158ab9f4b5b3d1aeb1d9ea178efc44cc11c2c08398621553cdc9.pdf)：射频能力和安全边界。公开手册把发射器定义为恒包络 FM 发射器，没有给出 SSB/DSB I/Q 发射控制。
- [BK4819 V3 寄存器表](https://alfaexploit.com/files/BK4819V3Registers_List_20201218.pdf)：只用于与本机 BK4829 现有驱动中同地址、同语义字段交叉核对。
- [BK4819 V3 Application Note](https://www.scribd.com/document/716113950/BK4819-V3-Application-Note-20210428-machine-translated-English)：用于核对 `REG_40` 发射频偏和 `REG_73` AFC 的公开字段定义。
- [uvk5cec 的公开 CW 说明](https://www.hamskey.com/2024/01/introducing-uv-k5-version-01p-cw.html)：只用于核对 A1A（载波开关键控）与 CWF/F2A（FM 音调）的行为区别；本项目未复制其固件实现。
- [fagci/uv-k5-firmware-fagci-mod](https://github.com/fagci/uv-k5-firmware-fagci-mod)：公开源码中的 USB/LSB `REG_47` 路径仅配置接收 AF，发射仍统一进入 FM TX link；用于排除把接收寄存器直接套到 TX 的错误路线。
- [sfotis/uv-k5-firmware-ssbtx，提交 354580f](https://github.com/sfotis/uv-k5-firmware-ssbtx/commit/354580f8159448de8f14861adc546c9ef5f93787)（Apache-2.0）：公开的实验性 DSB 技术路线，即读取 `REG_64` 话音幅度并连续改变 `REG_36` PA 偏置；本项目只移植这一公开思路并增加零频偏、无亚音/尾音和安全收发切换，未复制其菜单或存储布局。
- [losehu/uv-k5-firmware-custom](https://github.com/losehu/uv-k5-firmware-custom)等公开源码只用于确认 UV-K 系列现有实验路线；不同射频芯片或外接 SI4732 的实现没有直接移植到 BK4829。
- [M7OCM/890-II](https://github.com/M7OCM/890-II) Apache-2.0 开源源码：同 BK4819 系列 `REG_47` AF=4/5 的 LSB/USB 实测接收路径参考；公开寄存器表仍将这两个值列为保留，因此本项目保留 BK4829 真机验证要求。

未找到 Beken 公开发布的 BK4829 application note 或完整寄存器手册。因此未知寄存器语义不外推；资料不能证明的功能保持禁用并列入硬件待测项。没有使用或逆向 IJV 二进制、Radio Manager 或其他非公开实现。

## CN_RF 构建开关

| 开关 | 值 | 说明 |
|---|---:|---|
| `ENABLE_CN_RF` | ON | 本清单中的高级 RF 模式、菜单和独立配置区 |
| `ENABLE_WFM` | ON | 最小 BK1080 WFM 接收路径 |
| `ENABLE_FMRADIO` | OFF | 不编译旧 FM app、FM UI 和广播台存储；仅保留 BK1080 驱动 |
| `ENABLE_BYP_RAW_DEMODULATORS` | ON | BYP 及原有 RAW 接收路径 |
| `ENABLE_AIRCOPY` | ON | 保留现有 1024 信道 AirCopy |
| `ENABLE_FEAT_F4HWN_GAME` | OFF | 不编译电子木鱼、CW 练习器、原 Toolbox |
| `ENABLE_FEAT_F4HWN_PMR` | OFF | 删除 PMR 专用 TX Lock 项 |
| `ENABLE_FEAT_F4HWN_GMRS_FRS_MURS` | OFF | 删除 GMRS/FRS/MURS 专用 TX Lock 项 |
| `ENABLE_FEAT_F4HWN_CA` | OFF | 删除加拿大专用 TX Lock 项 |
| `ENABLE_FEAT_F4HWN_SCREENSHOT` | OFF | 按用户授权释放基础截图串流空间 |
| `ENABLE_DTMF_CALLING` | OFF | 不启用 DTMF 呼叫 UI；MDC1200 保留 |
| `ENABLE_SPECTRUM` / `ENABLE_FEAT_F4HWN_SPECTRUM` | ON | 保留频谱功能 |
| `ENABLE_CHINESE` | ON | 保留中英文、中文信道名、字库和拼音输入 |
| `ENABLE_USB` / `ENABLE_UART` | ON | 保留电脑联动基础设施 |

目标和 Edition 分别为 `Dondji.cn-rf`、`CN-RF`。

### 默认频率锁与兼容

- CN-RF 首次启动或损坏设置回退为 `F_LOCK_NONE`；完整恢复出厂会明确写入同一值。本任务没有执行恢复出厂。
- `F_LOCK` 的外置 Flash 编号固定为 0–10，不再随 CA/PMR/GMRS 编译开关重排；菜单通过独立映射隐藏已删除项。CN-RF 读到旧固件保存的已删除地区专用值时回退到 `F_LOCK_NONE`；其他预设仍回退 `F_LOCK_DEF`，且不会把旧值误解释成另一项。
- `F_LOCK_NONE` 只允许现有 `frequencyBandTable` 与 BK4829 接收检查已覆盖的频段，不扩大 VCO/滤波器/PA/校准范围。
- 普通 F Lock 菜单、每信道 `TX_LOCK`、PTT、TOT、BCL、供电/功率保护和原发射状态机均保留。

### 逐开关体积矩阵

以下均以同一次源码的 `Fusion` 预设（120400 B）为基线，每次只关闭一个开关；LTO 会跨模块优化，因此各项节省量不能直接相加。

| 单独关闭的开关 | `.bin` | 单项释放 |
|---|---:|---:|
| `ENABLE_FMRADIO` | 114560 B | 5840 B |
| `ENABLE_FEAT_F4HWN_GAME` | 117408 B | 2992 B |
| `ENABLE_FEAT_F4HWN_SCREENSHOT` | 119264 B | 1136 B |
| `ENABLE_FEAT_F4HWN_GMRS_FRS_MURS` | 120304 B | 96 B |
| `ENABLE_FEAT_F4HWN_CA` | 120328 B | 72 B |
| `ENABLE_FEAT_F4HWN_PMR` | 120376 B | 24 B |

`ENABLE_DTMF_CALLING` 在 Fusion 基线中已经为 OFF，因而没有可再释放的单项差值。CN-RF 最终镜像包含新增高级 RF、10 档带宽与最小 WFM 路径后仍比 Fusion 小 2964 B。

## 模式与接收链路

| 功能 | 菜单/显示 | 实现 | 状态 |
|---|---|---|---|
| FM、AM | `MODE` / 模式快捷切换 | 保留 Dondji 路径；高级配置在每次完整重配后重放 | 已编译，硬件回归待测 |
| USB、LSB | `MODE → USB/LSB` / `上边带/下边带` | BK4829 低中频接收，`REG_3D=0x2AAB`（公开表约 8.46 kHz IF）、`REG_47` AF=5/4；关闭 AM 解调、AFC 及 FM 静噪/亚音中断，基带音频常开 | 已实现并按信道/VFO 保存；边带抑制度和频率误差待信号源验证 |
| AMB | `MODE → AMB` / `广播调幅` | BK4829 AM 解调并旁路已定义的 RX HPF、LPF、去加重位 | 已实现，音质/占用带宽待测 |
| DSB | `MODE → DSB` / `双边带` | RX 使用 AF=5、零中频基带并同时接收两侧边带；TX 保留话筒 ADC，依次配置 `REG_46` 开启门限、`REG_79` 检测周期/关闭门限、`REG_7A` 延迟和 `REG_31[2]` 检测使能（不启用软件 VOX PTT），读取 `REG_64` 并以 48 MHz SysTick 为时基、按 4 kHz 名义节拍更新 `REG_36` PA 偏置；`REG_40=0`，禁用亚音、PTT ID、应答和尾音；TX 开始/结束先归零 PA，仍走 PTT、TX Lock、TOT、BCL、电池和功率保护 | **实验性收发已实现**；补齐了 CN_RF 关闭软件 VOX 时缺失的幅度检测初始化，并消除了无节制主循环更新。该包络法没有带符号 I/Q，相位和载波抑制度必须用仪表验证，不等同于可选 USB/LSB 发射 |
| CW | `MODE → CW` / `电报` | DSB 同源的零中频接收；键控器外直接按住 PTT 会开启校准功率的无调制载波；键控器内保持 BK4829 PLL/TX link 锁定并禁用麦克风 ADC、TX Mute、Tone1、亚音及偏移调制，点划以 2 ms 两级 PA 偏置边沿门控 `REG_36` 和 `GPIO1_PIN29_PA_ENABLE`，形成 A1A；仍走 PTT/TOT/BCL/功率保护；进入 CW 时默认 U6 | 已实现；TOT/低电等外部中止会完成通用 PTT 释放与 RX 状态恢复。包络、频谱和功率待假负载/仪表验证 |
| CW 键控器与信标 | CW 模式 `F+8` | 独立、非阻塞的国际 Morse A-Z/0-9/空格键控器；PTT 一次按下触发完整报文，松开不截断，再次按下可中止。`MENU` 切换 WPM/内容/信标，`▲/▼` 设置 5–30 WPM 或信标关/15–600 秒（每档 15 秒），数字键多击输入英文。专用 128×64 UI 阻止主状态栏二次覆盖；设置修改后延迟 2 秒合并写入，退出页面时立即刷新，避免连续按键反复擦写 4 KiB 扇区；信标在每轮发送完成后重新计时；默认关闭，所有发射仍走 TX Lock、TOT、BCL、电池和功率保护 | 已实现；需用假负载测量点划时序、功率和键控杂散 |
| WFM | `MODE → WFM` / `宽带调频` | 仅 76–108 MHz；BK4829 睡眠，最小 BK1080 驱动接收；越界回退 FM | 已实现，灵敏度/立体声不承诺 |
| BYP | `MODE → BYP` / `旁路` | BK4829 AF=9，旁路 RX/TX 音频滤波，供外部解码器取音频 | 已实现，电平/频响待测 |
| USB/LSB 发射 | 无 | BK4829 公开资料未给出可验证的 I/Q 或边带选择发射控制；`REG_47` 只选择接收 AF | **安全禁用**；DSB 使用上列独立的实验性 PA 包络法，不冒充 USB/LSB |

除 FM、CW 和实验性 DSB 外，CN-RF 模式均禁止发射。CW/DSB 结束时不发送 Roger、DTMF 或 CTCSS/DCS 尾音。模式切换、双守候和扫频的完整配置路径会重新设置调制、带宽、AGC、AFC和音频路径，避免保留前一模式寄存器；USB/LSB/DSB/CW 不进入会切断零中频音频的周期省电，Noise Blanker 结束后也按当前边带恢复 AF=4/5；WFM 期间停止 BK4829 中断、双守候和省电切换，拒绝 BK4829 扫频入口，离开时关闭 BK1080 并显式唤醒 BK4829。

## 高级 RF 控制

| 功能 | 菜单 | 持久化 | 寄存器/算法 | 状态 |
|---|---|---|---|---|
| 10 档带宽 | `BW` / `宽窄带` | 每 MR、每 VFO 频段 | BK4829 `REG_43`：W26/W23/W20/W17/W14/W12/N10/N9/U7/U6；W26 追加为存储值 9，旧 0–8 不重新编号 | 已实现，仪表待测；BK1080 WFM 不使用本设置 |
| AGC | `AGC` / `AGC模式` | 同上 | AUTO 使用芯片 AGC；MAN 使用 `REG_13` 固定增益；FAST/NORM/SLOW 用 RSSI 闭环调整经核对的 `REG_13` 增益组合 | 已实现，时间常数待实测 |
| RF Gain | `RF Gain` / `射频增益` | 同上 | 0–15，只有 MAN/FAST/NORM/SLOW 可改；AUTO 显示 N/A | 已实现 |
| RF Boost | `RF Boost` / `射频增强` | 同上 | 非 AUTO 时把 `REG_13` 前端组合增益表提高一级；可能联动 LNA/Mixer/PGA，与音频 EQ BOOST 无关，不虚称为单独 LNA 位 | 已实现，AUTO 显示 N/A |
| AFC | `AFC` / `自动频控` | 同上 | 0=关；1–8 按公开 Application Note 的 `REG_73[13:11]` AFC Range 与 `REG_73[4]` Disable 字段设置，从最小范围到最大范围 | 已实现 0–8；不触碰压扩寄存器 |
| Mic Gain | `Mic` / 原双语标题 | 同上 | BK4829 `REG_7D[5:0]`，沿用本项目校准增益表 | 已实现 |
| SetDEV | `SetDEV` / `发射频偏` | 同上 | BK4829 `REG_40[11:0]`；0 保持 Dondji 原有带宽相关值，1–9 是有界的直接控制字 | 已实现，1–9 均须频偏仪校准 |
| Noise Blanker | `NoiseB` / `噪声消隐` | 同上 | 读取已定义的 `REG_63` glitch 指示器，按三级阈值进行 20 ms 音频脉冲消隐；恢复当前模式的 AF 路由 | 已实现软件消隐，非虚构硬件位 |

压扩、扰频、CTCSS、DCS、偏移、功率、TOT、BCL 和 MDC1200 的既有字段及状态机没有迁移或替换。

## 初始总体任务中尚未完成的功能组

本轮实现范围是 CN_RF 基础裁剪、默认频率锁兼容，以及模式/接收链路/高级 RF 控制。以下初始需求尚未移植，不能标记为完成：

- F4 v5.8 的快速扫描/扫描进度/RSSI 曲线及其完整可靠性修复集；现有扫描与频谱仅做了 WFM 冲突保护。
- ARDF Beam/Foxhunt/Beacon、历史曲线、Geiger 音效和发射信标状态机。
- ZVEI/CCIR/EEA/Select-5 选择呼叫、联系人和接收过滤状态机；`ENABLE_DTMF_CALLING` 当前为 OFF，MDC1200 保留。
- IJV 风格 FastScan 预设、F Copy、Upconverter、QRA、工程校准菜单、PTT Toggle。
- 新电脑端 1024 信道导入/导出工具，以及 AirCopy 的 CN-RF 高级配置传输和显式布局版本握手。现有 AirCopy 的 8×128 信道频率、名称和属性路径仍保留。

真正的 USB/LSB 语音发射仍未开放：公开 BK4829 资料没有给出可验证的 I/Q/边带选择发射控制，不能用接收 AF 寄存器伪实现。DSB 采用公开实验路线，以话音幅度实时控制 PA 偏置；由于 `REG_64` 不是带符号 I/Q 样本，它只承诺实验性的对称边带输出，不承诺理想 DSB-SC 的相位反转或载波抑制度。CW 不依赖 I/Q 语音发射；它通过已定义的 PA-CTL 和板级 PA 使能对未调制载波作开关键控。

## 外置 Flash 占用审计

| 地址 | 内容 | 本次变化 |
|---|---|---|
| `0x000000–0x003FFF` | 1024 × 16 字节信道记录 | 未改变结构、地址或容量 |
| `0x004000–0x007FFF` | 1024 × 16 字节信道名槽 | 未改变 |
| `0x008000–0x00886D` | 1024 个 MR + 7 个 VFO 属性（至 `0x00880D`），以及 24 个扫描列表名称（`0x00880E–0x00886D`） | 未改变结构、地址或容量 |
| `0x00880E–0x00886D` | 24 个扫描列表名称 | 未改变 |
| `0x009000...` / `0x00A000...` | 原 VFO 和全局设置 | 只保留原模式高半字节；CN-RF 高级字段不占用保留位 |
| `0x010000...` | 原校准数据 | 未改变 |
| `0x020000–0x023FFF` | 旧中文名迁移区 | 未改变 |
| `0x024000–0x056236` | 中文字库、索引、拼音和版本 | 未改变 |
| `0x056237–0x05FFFF` | 审计后的空隙 | 保持未使用 |
| `0x060000–0x061FFF` | **CN-RF v1 独立配置区** | 新增 |
| `0x062000–0x062FFF` | **CW 键控器 v2 配置区**：带 CRC 的 WPM（5–30）与信标周期（关或 15–600 秒）；兼容读取 v1 的 WPM；修改后延迟 2 秒或退出时合并保存 | 新增；未知版本拒绝覆盖 |
| `0x1FF000...` | 原自定义 Logo | 未改变 |

CN-RF v1 包含 16 字节签名/版本/记录尺寸/数量/CRC 头，以及 `(1024 + 7×2) × 6 = 6228` 字节记录，总计 6244 字节，两个 4 KiB 扇区内保留 1948 字节。每条记录有 CRC8。空白区首次保存时只写 v1 头；未知版本拒绝写入，避免降级破坏。保存前比较旧记录，未变化时不写；发生变化时沿用 `PY25Q16_WriteBuffer` 的 4 KiB 扇区缓存读改写，不做不安全的 NOR 就地覆盖。只有用户执行完整恢复出厂时才擦除这两个新扇区。本任务没有执行恢复出厂操作。

## 静态与构建验证

- `cmake --preset CN_RF`：通过。
- `cmake --build --preset CN_RF --parallel`：通过。
- CN-RF 使用 `-flto-partition=one` 做跨源文件尺寸去重，不影响其他预设。
- ELF：text 120272 B、data 548 B、bss 13576 B；链接器 FLASH 120824 B、RAM 14120 B。
- `Dondji.cn-rf.bin`：120824 B；上限 120832 B；余量 8 B。
- 构建图只包含最小 `driver/bk1080.c`，不包含旧 `app/fm.c`、`ui/fmradio.c`、游戏、CW 练习器、Toolbox 或截图模块。
- 原有预设回归构建通过：Fusion（本轮复测 FLASH 120632 B / RAM 15184 B）、Game（FLASH 113392 B / RAM 13584 B）、Broadcast（FLASH 111356 B / RAM 13544 B）、Custom（FLASH 101656 B / RAM 12240 B）、Bandscope（FLASH 113128 B / RAM 14912 B）、Basic（FLASH 112564 B / RAM 13880 B）、RescueOps（FLASH 108088 B / RAM 13416 B）。
- 编译/静态检查不能替代真机射频验证。刷写前应备份整个外置 Flash 与校准区；发射测试必须接假负载、功率计、频偏仪和频谱仪。

## 真机验收清单

1. 两个 VFO 与 MR/VFO 来回切换所有模式，检查静音、双守候、扫描暂停/恢复、提示音后的音频恢复。
2. 用已校准信号源逐档测量 10 档 IF/音频带宽（特别确认 W26 的实际 -6 dB/-60 dB 占用带宽）、RSSI、AGC 攻击/释放和 RF Gain/LNA 饱和点。
3. 用 AM、USB、LSB、CW 信号源确认 AMB/DSB/CW 的频率偏置、音质及零中频直流噪声；分别验证 USB 的 AF=5、LSB 的 AF=4、边带抑制度、载频误差容限，并确认 Noise Blanker、双守候和提示音后仍恢复正确边带；DSB 不应被宣传为单边带选择滤波。
4. 用 76–108 MHz WFM 信号源验证 BK1080 灵敏度、静音、提示音恢复和越界回退。
5. 用声卡/隔离线验证 BYP 对 APRS/AIS/SSTV 外部解码；不要把 BYP 表述为机内数字协议。
6. CW/DSB 发射仅接假负载：CW 用示波器/频谱仪验证 A1A 点划时序、载波无音调边带、键控包络、TOT、BCL、低电/过压保护、PA 功率和杂散；DSB 以 300 Hz、1 kHz、2.5 kHz 单音逐项测量载波抑制、上下边带对称度、总占用带宽、IMD、功率峰值、PA 电流和热稳定性，并确认静音时 PA 偏置回到零。USB/LSB/BYP/WFM/AM/AMB 仍须拒绝发射。
7. 用频偏仪逐档校准 Mic Gain/SetDEV；未完成测量前不要把插值档作为生产标定值。
8. 保存/重启后抽检 MR 0、127、128、1023 和两个 VFO 的全部高级字段；核对中文名、扫描列表、MDC1200、字体和拼音输入未变化。
