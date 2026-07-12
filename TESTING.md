# 测试记录

日期：2026-06-17

本轮目标：做一次本地自用收口，并新增 `LaptopPowerFeature` 笔记本总功耗检测功能。原则是小范围修改、保持可编译、不引入联网和强制驱动流程。

## 构建环境

- 项目目录：`work/fps-overlay-main`
- 使用工作区内置 MSVC：
  - `work/msvc/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe`
  - `work/msvc/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe`
- 输出文件：`work/fps-overlay-main/build/overlay/overlay.exe`

## 已完成验证

### 1. 本地自用启动收口

改动：

- 默认关闭启动联网检查更新。
- 启动时不请求 GitHub。
- 不再请求原作者仓库。
- PawnIO 改为可选：没有 PawnIO 时程序仍然启动，相关传感器显示 `N/A`。
- 保留手动“安装 PawnIO”按钮，但不会启动时强制安装、强制退出或自动重启电脑。
- 修复 `g_Config.autoStart = true` 强制覆盖配置的问题，用户配置里的 `autoStart` 现在可以生效。
- 保留现有 `features/` 自用扩展框架，没有把示例功能塞回 `main.cpp`。

验证：

- Release 编译通过。

## v1.10.0 中文化与功耗扫描测试

中文界面检查：

- Laptop Power 功能名称和全部选项已改为简体中文。
- 采样间隔、平滑平均、悬浮窗模式等下拉项已汉化。
- Overlay 中电源、整机、输入、电池、已知组件、来源均为中文。
- 普通错误弹窗和诊断导出失败提示已汉化。
- 常用快捷键名称已汉化。
- 中文字体使用微软雅黑或黑体完整字形范围。

功耗传感器检查：

- 启动后扫描全部 LHWM `Power` 传感器。
- 本机当前扫描结果包含 CPU Package、CPU Platform、核显功耗和独显 Package 等组件传感器。
- 自动匹配不会把 CPU Platform 自动标记为整机总功耗。
- 候选项悬停时显示路径和实时瓦数。
- 重新扫描不会安装驱动、不会请求网络、不会强制 PawnIO。
- CPU + GPU 合计仍只显示为“已知组件”，不冒充整机功耗。

### 2. LaptopPowerFeature

新增：

- `features/LaptopPowerFeature.h`
- `features/LaptopPowerFeature.cpp`

改动：

- 扩展 `FeatureContext`，加入 AC、电池充电/放电、估算整机功耗、CPU Package Power、GPU Power 等字段。
- 注册到 `FeatureRegistry`，默认启用。
- 设置页可开启/关闭，可调整显示、采样间隔、平滑平均、阈值和显示模式。
- Overlay 支持 Mini / Normal / Full 三种显示。
- 读取失败时显示 `N/A`，不会崩溃。
- 大于 500W 或无效功率值会被过滤。

数据来源：

- Windows Battery API / DeviceIoControl：读取电池充电/放电功率。
- LibreHardwareMonitor：复用现有传感器，读取 CPU Package Power、GPU Power、AC Adapter、Battery、Total System 等可用功率项。
- ASUS / G-Helper / EC：本轮不新增强制专用驱动，只保留未来扩展位置。

验证：

- Release 编译通过。

## 手动测试清单

- 插电状态下显示输入功率或 `N/A`。
- 电池状态下显示放电功率或 `N/A`。
- 充电时显示电池充电功率或 `N/A`。
- 读不到总输入功率时程序不崩溃。
- CPU/GPU 功耗能显示时，Full 模式中标记为组件功耗，不冒充整机总功耗。
- Overlay Mini / Normal / Full 显示正常。
- 设置页开关生效。
- 采样间隔 1000ms / 2000ms / 5000ms 生效。
- 平滑平均 Off / 5 seconds / 10 seconds 生效。
- 没有 PawnIO 时程序仍可启动。
- 启动时不弹出 PawnIO 强制安装、强制重启或强制退出流程。
- 用户关闭 `autoStart` 后，下一次启动仍按配置进入设置窗口。

## 本轮编译结果

通过：

- `rc.exe` 资源编译通过。
- `cl.exe` C++ 编译与链接通过。

未完成：

- 未在真实游戏内做长时间运行测试。
- 未覆盖所有笔记本厂商的电池/适配器传感器命名。
- 风扇和 EC 专用传感器仍依赖现有读取能力，本轮没有新增强制驱动。

## 2026-06-18 LaptopPowerFeature 显示优化

已确认当前机器行为：

- 拔掉充电线后，Windows Battery API 可以读取 Battery Discharge Rate。
- 插电后，系统没有暴露可用的 AC Input Power 传感器。
- CPU 与 GPU 功耗只作为 `Known Components` 显示，不代替整机功耗。

本轮验证：

- 电池模式下，`System Estimate` 使用 Battery Discharge Rate。
- Mini 模式显示 `Power: xxW`；插电且 AC Input 不可用时显示 `Power: N/A`。
- Normal 模式显示 AC 状态、System、Battery 和 Known Components。
- Full 模式显示 AC Input、System Estimate、电池充放电、CPU、GPU、Known Components、Source 和 Failed Reason。
- 插电且 AC Input 不可用时显示 `AC input sensor not exposed`。
- 电池 API 返回 0W 且状态可读时，插电状态显示 `Battery: full / idle`。
- Debug 输出区分 Battery discharge available、AC input available 和 AC input failure reason。
- 电源模式切换时重置平滑平均，避免拔插电后沿用旧模式数值。
- Release 编译通过。

手动复测：

- 插电：确认 `AC Input: N/A`、`System Estimate: N/A` 和失败原因正常。
- 拔电：确认放电功率显示为 `-xxW`，System Estimate 与放电功率一致。
- CPU/GPU 有数据时：确认只显示为 `Known Components`。
- Mini / Normal / Full：确认布局和状态文字正常。

## 2026-06-18 Overlay 对齐修复

- Mini 模式的 Power 与 Known 改为同一行显示。
- Normal 模式的 AC、System、Battery、Known 改为统一间距的单行横向显示。
- 移除 Mini / Normal 模式前的额外分隔线和纵向间距，避免覆盖层向下遮挡画面。
- Full 模式继续保留详细多行布局。
- Release 编译通过。

## 2026-06-18 Game++ 原生状态栏接入

- 新增通用 `InlineOverlayMetric`，Feature 只提供指标数据，不自行决定 Game++ UI。
- `FeatureRegistry::GetInlineOverlayMetrics()` 汇总功能指标。
- Game++ 布局复用现有 `drawSeg()` 绘制 Laptop Power。
- 未修改原有 FPS、CPU、GPU、显存、内存指标的绘制顺序与逻辑。
- LaptopPowerFeature 的 `DrawOverlay()` 为空，不再创建或绘制独立多行区域。
- Mini 手动检查：只追加一个 `功耗` 或 `已知` 分段。
- Normal 手动检查：追加 `电源`、`系统`、`已知` 分段。
- Full 手动检查：追加 `电源`、`输入`、`系统`、`电池`、`已知`、`来源`分段。
- 插电且 AC Input 不可用时应显示 `电源 AC`、`系统 N/A`、`已知 xxW`。
- 电池放电率可用时应显示 `电源 电池`、`系统 xxW`，Full 模式显示 `电池 -xxW`。
- Release 编译通过。

## 2026-06-18 整机功耗独立显示开关

- “自用功能”→“笔记本功耗”新增“显示整机功耗”复选框。
- 默认开启，配置项为 `[Features] laptop_power.show_system_power=1`。
- 开启时 Overlay 显示“整机 xxW”或“整机 N/A”。
- 关闭时 Overlay 不再生成“整机”指标，因此不会出现“整机 N/A”。
- 关闭后自动整机功耗扫描和设置页调试信息仍然工作。
- 关闭后 CPU/GPU 功耗读取不受影响，“已知 xxW”仍可显示。
- 保存配置并重启后，开关状态保持不变。
- Release x64 资源编译、C++ 编译和链接通过。

## 2026-06-18 已知组件功耗独立显示开关

- “自用功能”→“笔记本功耗”新增“显示已知组件功耗”复选框。
- 默认开启，配置项为 `[Features] laptop_power.show_known_components_power=1`。
- 开启且 CPU/GPU 功耗可用时，Overlay 显示“已知 xxW”。
- 关闭时 Overlay 不再生成“已知”指标。
- 关闭后 CPU/GPU 功耗读取和设置页“已知组件功耗”调试信息保持工作。
- 关闭后自动整机功耗检测及“整机”指标不受影响。
- 保存配置并重启后，开关状态保持不变。
- Release x64 资源编译、C++ 编译和链接通过。

## 2026-06-18 整机功耗与实时输入回退

本机诊断：

- LibreHardwareMonitor 扫描到 7 个 `Power` 传感器。
- 扫描结果只有 CPU Package/Cores/Memory/Platform、核显和独显组件功耗。
- 没有 AC Input、Adapter、System、Board 或 EC Power，因此严格自动匹配必然显示 N/A。
- Windows `Power Meter` 已注册，但当前实时值为 0，不能作为有效来源。
- AIDA64 配置中存在 `PDCIN`、`PPWR1..4` 和 `PBATTCHR` 等功率项。

改动验证：

- 新增 AIDA64 官方共享内存 `AIDA64_SensorValues` 读取。
- 自动识别 `PDCIN` 和输入/适配器类功率标签。
- 自动识别 Total System、System、Platform、Board 和 EC Power。
- Overlay 新增“输入 xxW / N/A”，未改变现有横向分段绘制函数。
- 只有输入功耗时，整机使用输入减电池充电功率；没有充电功率时使用输入值。
- CPU+GPU 仍只显示为“已知”，不参与整机计算。
- 设置页新增“实时输入功耗：xxW / N/A”调试行。
- AIDA64 共享内存不存在时读取函数静默返回，LHM/ASUS/电池逻辑继续工作。
- Release x64 资源编译、C++ 编译和链接通过。

实机复测前置：

- 关闭并重新启动 AIDA64。
- 在“首选项 → 硬件监控 → 外部应用程序”确认“启用共享内存”已开启。
- 插电时确认设置页“实时输入功耗”和 Overlay“输入”出现有效瓦数。
- 电池充电时确认整机值小于输入值，差值接近电池充电功率。
# 最近验证项

- [ ] 未开启 AIDA64 共享内存时，FPSOverlay 不结束、不修改、不重启 AIDA64。
- [ ] 开启 AIDA64 共享内存并重启 AIDA64 后，FPSOverlay 可读取 `AIDA64_SensorValues` 中的功耗值。
- [ ] `laptop_power.allow_estimated_system_power=0` 且无真实整机来源时，悬浮窗显示“整机 N/A”。
- [ ] `laptop_power.allow_estimated_system_power=1` 时，可显示“整机 ≈xxW”，来源为“本机传感器融合估算”。
- [ ] “已知组件功耗”在估算开关关闭时仍正常显示。
- [ ] 游戏加加风格横向悬浮窗布局未改变。

## 传感器融合估算验证

- [ ] 无真实整机传感器且允许估算时，Overlay 显示 `整机 ≈xxW`。
- [ ] 设置页自动来源显示“本机传感器融合估算”。
- [ ] 设置页显示 CPU/平台、独显、风扇、SSD、网络、主板/屏幕和损耗的估算构成。
- [ ] 核显功耗不会与 CPU Package 重复相加。
- [ ] CPU Platform 只有在数值可合理覆盖 CPU Package 时才作为更宽功耗域使用。
- [ ] CPU/GPU 负载和风扇转速变化时，整机估算值会相应变化。
- [ ] 关闭估算开关后，无真实来源仍显示 `整机 N/A`。

### G815LR 专用模型验证

- [ ] 系统型号识别为 `G815LR`，自动来源显示“枪神9 Plus G815LR 专用融合模型”。
- [ ] 切换界面监控到 Intel 核显后，整机估算仍包含 RTX 5070 Ti 功耗。
- [ ] 调整 Windows 屏幕亮度后，设置页中的屏幕估算功耗随之变化。
- [ ] CPU Package、CPU Platform、CPU Memory和核显没有重复相加。
- [ ] RTX 5070 Ti、CPU和最终整机功耗没有人为上限截断。
- [ ] 模型结果可以超过 280W，并继续按实时分部件数据变化。
- [ ] 电池模式运行一段时间后，自校准修正量逐渐稳定。
- [ ] 设置页显示模型置信度和预计误差。

### 电池辅助供电监控

- [ ] “显示电池放电功耗”可独立控制悬浮窗指标。
- [ ] 纯电池供电时正常显示电池放电瓦数。
- [ ] 插电且增强模式从电池补电时，仍能显示实时放电瓦数。
- [ ] 插电放电时设置页显示“插电状态下电池辅助供电”。
- [ ] 没有放电数据时显示 N/A，不生成虚假瓦数。

## 性能与设置响应验证

- [ ] 右下角托盘图标选择“设置”后，设置面板在同一主循环迭代出现。
- [ ] 覆盖层隐藏时打开设置，不再额外等待固定 50ms。
- [ ] 日志中的 `Live settings first frame` 延迟稳定且明显降低。
- [ ] 设置面板打开期间可以立即点击、切换标签和修改选项。
- [ ] 普通显示状态 CPU 占用较旧版下降，拖动和设置操作仍保持流畅。
- [ ] 中文设置项没有缺字或方框。

## 显存与内存独立格式验证

- [ ] 只开启显存百分比时，仅显示 `显存 xx%`。
- [ ] 只开启显存已用/总量时，仅显示 `显存 x.x/xxG`。
- [ ] 只开启内存百分比时，仅显示 `内存 xx%`。
- [ ] 只开启内存已用/总量时，仅显示 `内存 x.x/xxG`。
- [ ] 同时开启两项时，两种数值同时显示。
- [ ] 四种布局中的开关行为一致。

## 2026-06-20 功耗模型诊断与修正

真实数据结论：

- 发布目录日志显示 LHWM 仅发现 7 个组件功耗传感器，没有 AC 输入或整机功耗传感器。
- Windows `Power Meter(Power Meter (0))\Power` 连续采样为 `0W`，不能作为真实整机来源。
- Windows `Energy Meter(RAPL_Package0_PKG)\Power` 实测约 `57.8W–62.5W`，属于 CPU Package/RAPL，不是整机功耗。
- 当前诊断文件确认可读取 CPU Package、CPU Memory、CPU Platform、Intel 核显和 RTX 5070 Ti Package Power。
- 当前运行时没有 AIDA64 进程，因此旧日志中没有 AIDA64 共享内存成功样本。

已修正：

- [x] 平滑值按模型采样周期更新，不再每帧覆盖 5 秒平滑窗口。
- [x] 校准样本按模型采样周期累计，不再每帧虚增样本数和置信度。
- [x] 插电辅助供电公式计入 `电池放电`，并扣除 `电池充电`。
- [x] 插电和电池模式使用独立校准状态。
- [x] 适配器/输入功率不再进入真实整机传感器候选。
- [x] 校准偏移没有固定功耗上限，最终整机估算仍可超过 280W。
- [x] AIDA64 来源可用/失效日志和周期模型快照已加入。
- [x] 诊断包已加入当前模型来源、置信度、预计误差和构成。
- [x] 游戏加加横向指标仍通过原 `drawSeg()` 绘制，布局代码未改。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。

后续实机复测：

- [ ] 开启 AIDA64 共享内存后，日志出现 `AIDA64 power shared memory active`。
- [ ] 关闭 AIDA64 后，日志出现来源失效记录，并平滑回退到融合模型。
- [ ] 插电且电池辅助放电时，整机估算接近 `DC 输入 + 电池放电 - 电池充电`。
- [ ] 5 秒平滑在持续负载突变时跨多个采样点收敛，不再在数帧内失效。
- [ ] 插电和电池各运行至少 1 分钟，确认两套校准样本与偏移分别稳定。
- [ ] 日志 `Power model snapshot` 的来源、置信度和预计误差与设置页一致。

## v1.10.1 版本标识验证

- [x] 程序内版本标识为 `v1.10.1 (2026-06-20 19:25)`。
- [x] Windows `FileVersion` 为 `1.10.1.0`。
- [x] Windows `ProductVersion` 为 `v1.10.1 (2026-06-20 19:25)`。
- [x] 应用清单版本同步为 `1.10.1.0`。
- [x] Release 发布目录同时提供 `overlay.exe` 和 `overlay_v1.10.1_20260620-1925.exe`。

## v1.10.2 设置界面分区验证

- [x] 实时设置代码划分为监控项目、外观布局、控制、硬件、提醒与功耗五个标签页。
- [x] FPS、CPU、GPU、内存/存储和网络选项分别放入独立卡片。
- [x] 功耗设置拆分为显示、估算与提醒、实时状态、传感器工具。
- [x] 设置主题使用 Push/Pop 限定作用域，不影响 Overlay 后续绘制。
- [x] 游戏加加 `drawSeg()`、指标顺序和横向布局代码未修改。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [ ] 手动打开五个标签页，确认不同 DPI 下无文字截断或控件重叠。
- [ ] 手动滚动功耗卡片，确认置信度、估算构成和传感器按钮完整可见。
- [ ] 游戏中打开设置并关闭，确认 Overlay 样式在设置前后保持一致。

## v1.10.2 版本标识验证

- [x] 程序内版本标识为 `v1.10.2 (2026-06-20 20:20)`。
- [x] Windows `FileVersion` 为 `1.10.2.0`。
- [x] Windows `ProductVersion` 为 `v1.10.2 (2026-06-20 20:20)`。
- [x] 应用清单版本同步为 `1.10.2.0`。

## v1.10.3 开机自启动验证

- [x] 新增 `[App] startWithWindows` 配置键，旧配置保持兼容。
- [x] 旧版 `AutoLaunchTaskPath` 可迁移为开关开启状态。
- [x] 新安装且没有旧任务路径时默认关闭，不再无条件创建计划任务。
- [x] 开启时创建当前用户登录触发器，并使用最高权限运行。
- [x] 计划任务允许电池供电启动，并设置为电池状态下继续运行。
- [x] 关闭时删除 `FPS Overlay` 计划任务并清空旧路径标记。
- [x] 标准设置页和完整启动设置页均提供同一个开关及实际状态。
- [x] 诊断包记录配置状态和计划任务实际状态。
- [ ] 手动关闭开关后，使用任务计划程序确认 `FPS Overlay` 任务已删除。
- [ ] 手动开启开关后，注销并重新登录，确认程序以管理员权限启动。
- [ ] 移动发布目录后启动一次，确认计划任务执行路径自动更新。

## v1.10.3 科技主题验证

- [x] 设置主题调整为深海军蓝、霓虹青和靛紫。
- [x] 卡片、标签、按钮、滑块、滚动条和表头使用统一色板。
- [x] 主题样式栈使用成对 Push/Pop，不泄漏到 Overlay 绘制。
- [x] 游戏加加 `drawSeg()`、指标顺序和横向布局代码未修改。
- [ ] 手动检查不同 DPI 下的标题光条、卡片边框和文字对比度。

## v1.10.3 版本标识验证

- [x] 程序内版本标识为 `v1.10.3 (2026-06-20 20:30)`。
- [x] Windows `FileVersion` 为 `1.10.3.0`。
- [x] Windows `ProductVersion` 为 `v1.10.3 (2026-06-20 20:30)`。
- [x] 应用清单版本同步为 `1.10.3.0`。

## v1.10.4 侧边栏与自由缩放验证

- [x] 启动设置页与实时设置共用六分区侧边栏和卡片内容。
- [x] 旧设置页中的 GPU 选择、CPU/GPU 频率来源、诊断包和 PawnIO 工具已保留。
- [x] 主窗口可调整为 `1200×800` 和 `930×660`，尺寸与请求一致。
- [x] 请求低于最小尺寸时稳定限制为 `720×520`。
- [x] 窄窗口自动缩小侧边栏，内容卡片切换为单列并保持纵向滚动。
- [x] 启动页 Overlay 状态显示为“尚未启动”，不再误报“正在显示”。
- [x] 启动 Overlay 按钮位于侧边栏顶部，在窗口缩放后仍可访问。
- [x] 界面验证构建跳过管理员硬件采集；正式 Release 保持完整传感器初始化。
- [x] 游戏加加 `drawSeg()`、横向指标顺序和功耗项接入未修改。

## v1.10.4 版本标识验证

- [x] 程序内版本标识为 `v1.10.4 (2026-06-20 21:09)`。
- [x] Windows `FileVersion` 为 `1.10.4.0`。
- [x] Windows `ProductVersion` 为 `v1.10.4 (2026-06-20 21:09)`。
- [x] 应用清单版本同步为 `1.10.4.0`。

## v1.10.5 科技风精修验证

- [x] 控制中心状态轨道完整显示 Overlay、传感器、整机功耗和当前布局。
- [x] 首页左右列独立堆叠，不再出现由表格行高引起的大块空白。
- [x] 六个侧边栏页面均可点击进入，标题编号和选中状态同步。
- [x] 监控项目、游戏内监控、控制与启动、硬件信息、提醒与功耗页面内容完整显示。
- [x] 游戏内监控页三个滑杆标签均位于控件上方，没有右侧截断。
- [x] 宽窗口 `1960×1330`、窄窗口 `1500×1050` 和最小窗口 `1260×910` 实机截图通过。
- [x] 最小窗口使用紧凑品牌区、短版本号和单行导航。
- [x] Tab 键可进入控件，当前焦点使用天蓝色边框显示。
- [x] 主题 Push/Pop 数量匹配，Release/QA 编译未出现样式栈断言。
- [x] QA 构建不会实际启动 Overlay；正式构建保留启动按钮原逻辑。
- [x] 游戏加加 `drawSeg()`、横向指标顺序和 Overlay 绘制样式未修改。

## v1.10.5 版本标识验证

- [x] 程序内版本标识为 `v1.10.5 (2026-06-21 12:32)`。
- [x] Windows `FileVersion` 为 `1.10.5.0`。
- [x] Windows `ProductVersion` 为 `v1.10.5 (2026-06-21 12:32)`。
- [x] 应用清单版本同步为 `1.10.5.0`。

## v1.10.6 托盘菜单失焦关闭验证

- [x] 托盘菜单调用 `TrackPopupMenu` 前显式执行 `SetForegroundWindow`。
- [x] 保留 `TPM_RETURNCMD | TPM_NONOTIFY` 命令返回模式和菜单关闭后的 `WM_NULL`。
- [x] 独立托盘线程、功耗子菜单、设置和退出命令逻辑未改。
- [ ] 实机右键打开托盘菜单后点击桌面空白处，确认菜单立即消失。
- [ ] 实机展开“功耗显示”子菜单后点击菜单外部，确认父子菜单均立即消失。

## v1.10.6 版本标识验证

- [x] 程序内版本标识为 `v1.10.6 (2026-06-21 13:04)`。
- [x] Windows `FileVersion` 为 `1.10.6.0`。
- [x] Windows `ProductVersion` 为 `v1.10.6 (2026-06-21 13:04)`。
- [x] 应用清单版本同步为 `1.10.6.0`。

## v1.10.7 仅桌面显示验证

- [x] 新增 `[Layout] desktopOnlyMode`，旧配置缺少该键时默认关闭。
- [x] 全屏检测忽略桌面、任务栏、隐藏窗口和最小化窗口。
- [x] 前台窗口覆盖其所在显示器时临时隐藏 Overlay，不修改用户手动显示状态。
- [x] 退出全屏后按原手动显示状态恢复。
- [x] 打开设置面板时不执行自动隐藏。
- [x] 检测周期为 200ms，隐藏状态下仍持续检测恢复条件。
- [x] 游戏加加 `drawSeg()`、横向指标顺序和渲染样式未修改。
- [ ] 实机开启开关后进入浏览器 F11 全屏，确认 Overlay 在约 200ms 内隐藏。
- [ ] 退出浏览器 F11 全屏，确认 Overlay 自动恢复。
- [ ] 进入无边框全屏游戏，确认 Overlay 自动隐藏。
- [ ] 手动隐藏 Overlay 后进入并退出全屏，确认 Overlay 不会自行显示。
- [ ] 多显示器环境下，在副屏全屏应用并切换前台窗口，确认按前台应用所在屏幕判断。

## v1.10.7 版本标识验证

- [x] 程序内版本标识为 `v1.10.7 (2026-06-22 17:41)`。
- [x] Windows `FileVersion` 为 `1.10.7.0`。
- [x] Windows `ProductVersion` 为 `v1.10.7 (2026-06-22 17:41)`。
- [x] 应用清单版本同步为 `1.10.7.0`。

## v1.10.8 最大化检测修正

- [x] 普通 Windows 最大化状态通过 `IsZoomed` 检测。
- [x] 覆盖显示器工作区 `rcWork` 的前台窗口会触发自动隐藏。
- [x] 覆盖完整显示器 `rcMonitor` 的全屏窗口仍会触发自动隐藏。
- [x] 桌面、任务栏、隐藏窗口和最小化窗口继续被排除。
- [ ] Edge 普通最大化后 Overlay 在约 200ms 内隐藏，恢复窗口后自动显示。
- [ ] 资源管理器普通最大化后行为与 Edge 一致。
- [ ] 浏览器 F11、视频全屏和无边框全屏游戏继续正常隐藏。

## v1.10.8 版本标识验证

- [x] 程序内版本标识为 `v1.10.8 (2026-06-22 17:49)`。
- [x] Windows `FileVersion` 为 `1.10.8.0`。
- [x] Windows `ProductVersion` 为 `v1.10.8 (2026-06-22 17:49)`。
- [x] 应用清单版本同步为 `1.10.8.0`。

## v1.10.10 桌面显示独立设置项验证

- [x] 侧边栏新增“桌面显示”独立页面，页面编号为 `04`。
- [x] “控制与启动”“硬件信息”“提醒与功耗”页面编号顺延为 `05`、`06`、`07`。
- [x] “仅桌面显示”开关从“控制与启动”页迁移到“桌面显示”页。
- [x] 配置仍使用 `[Layout] desktopOnlyMode`，旧配置状态保持兼容。
- [x] 页面显示未启用、桌面显示中、前台应用占满屏幕时临时隐藏三种状态。
- [x] 最大化、工作区覆盖、全屏检测及手动显示状态分离逻辑未修改。
- [x] 游戏加加 `drawSeg()`、横向指标顺序、功耗项接入和 Overlay 样式未修改。
- [ ] 实机打开“桌面显示”页，切换开关并重启，确认状态持久化。
- [ ] Edge 最大化和恢复后，确认 Overlay 继续按原逻辑隐藏与恢复。

## v1.10.10 版本标识验证

- [x] 程序内版本标识为 `v1.10.10 (2026-06-22 18:25)`。
- [x] Windows `FileVersion` 为 `1.10.10.0`。
- [x] Windows `ProductVersion` 为 `v1.10.10 (2026-06-22 18:25)`。
- [x] 应用清单版本同步为 `1.10.10.0`。

## v1.10.11 游戏优先显示修正

- [x] 前台 PID 更新移到自动隐藏之前，Overlay 隐藏后仍会持续识别前台程序。
- [x] ETW 帧数据在自动隐藏期间继续采集，确认游戏后可以自动恢复显示。
- [x] 游戏安装路径和 `-Win64-Shipping` / `-Win32-Shipping` 等特征可直接确认游戏。
- [x] 未知全屏程序先保持显示并等待渲染证据，不会在切入游戏时立即隐藏。
- [x] 浏览器、办公、聊天、游戏平台启动器和媒体程序使用明确的普通应用判定。
- [x] 前台进程持续产生有效帧 600ms 后确认为游戏，短暂桌面合成帧不会立即触发。
- [x] 前台目标清空时同步清空旧 FPS 和进程名，避免桌面显示上一程序的陈旧状态。
- [x] `FeatureContext::isInGame` 只使用确认后的前台游戏状态。
- [x] 桌面显示页可反馈识别中、游戏保持显示和普通应用临时隐藏状态。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] 游戏加加 `drawSeg()`、横向指标顺序、功耗项接入和 Overlay 样式未修改。
- [ ] 实机最大化 Edge，确认约 500ms 后 Overlay 临时隐藏。
- [ ] 实机进入无边框或独占全屏游戏，确认 Overlay 始终保持或在识别后恢复显示。
- [ ] 从普通应用全屏直接切换到游戏，确认无需手动操作即可恢复。
- [ ] 使用不在常见游戏平台目录中的独立游戏，确认实时帧证据可以正确识别。

## v1.10.11 版本标识验证

- [x] 程序内版本标识为 `v1.10.11 (2026-06-22 21:06)`。
- [x] Windows `FileVersion` 为 `1.10.11.0`。
- [x] Windows `ProductVersion` 为 `v1.10.11 (2026-06-22 21:06)`。
- [x] 应用清单版本同步为 `1.10.11.0`。

## v1.10.12 界面层级与托盘开关

- [x] 状态卡片高度由字体行高、上下内边距和行间距动态计算。
- [x] 高 DPI 下两行状态文字不再压住卡片底边或下一行卡片。
- [x] 打开 Overlay 设置中心时调用 `HWND_NOTOPMOST`，设置窗口使用普通层级。
- [x] 设置中心关闭时使用不抢焦点的 `HWND_TOPMOST` 恢复 Overlay 原行为。
- [x] 托盘菜单新增带勾选状态的“仅桌面显示”快捷项。
- [x] 托盘开关通过主窗口消息修改配置，避免托盘线程直接写配置。
- [x] 切换后立即保存 `[Layout] desktopOnlyMode`，设置页与托盘状态同步。
- [x] QA x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] 游戏加加绘制区及功能模块未修改。
- [ ] 实机在 125%、150%、175% 缩放下确认四个状态卡片无文字重叠。
- [ ] 打开设置中心后切换到其他应用，确认设置中心不会保持在最前面。
- [ ] 关闭设置中心后确认 Overlay 在游戏中恢复置顶。
- [ ] 托盘切换“仅桌面显示”后重新打开菜单，确认勾选状态和设置页一致。

## v1.10.12 版本标识验证

- [x] 程序内版本标识为 `v1.10.12 (2026-06-22 21:19)`。
- [x] Windows `FileVersion` 为 `1.10.12.0`。
- [x] Windows `ProductVersion` 为 `v1.10.12 (2026-06-22 21:19)`。
- [x] 应用清单版本同步为 `1.10.12.0`。

## v1.10.13 G815LR 插座功耗校准

- [x] 录入 15 组软件估算与插座电力监测仪配对数据。
- [x] 原始估算对全部样本的平均绝对误差约为 `31.1W`。
- [x] `240W → 208W` 与相邻 `220W → 250W` 不满足同一单调稳态关系，按负载切换时间错位样本降权。
- [x] v1.10.13 使用其余 14 组数据拟合 `0.9277145 × 软件估算 + 43.236W`；该历史公式已由 v1.10.18 新实测公式替代。
- [x] 稳定样本校准后平均绝对误差约为 `3.0W`。
- [x] `78W` 校准为约 `115.6W`，覆盖低负载实测 `115–116W`。
- [x] `220W` 校准为约 `247.3W`，覆盖高负载实测 `247–250W`。
- [x] 校准仅作用于 G815LR 插电融合估算，不修改真实传感器、真实输入功率和电池放电来源。
- [x] 动态偏移校准改为以插座校准结果为基准学习残差。
- [x] 来源文字和估算构成显示“G815LR 插座实测校准”及公式。
- [x] 不增加固定功耗上限。
- [x] QA x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [ ] 使用电力监测仪复测 `80W`、`150W`、`220W` 三档稳定负载，各保持至少 30 秒并对齐时间。
- [ ] 游戏加载、场景切换和退出阶段分别记录软件值与电力表值，确认瞬时样本不会被当作稳态校准依据。

## v1.10.13 版本标识验证

- [x] 程序内版本标识为 `v1.10.13 (2026-06-23 14:08)`。
- [x] Windows `FileVersion` 为 `1.10.13.0`。
- [x] Windows `ProductVersion` 为 `v1.10.13 (2026-06-23 14:08)`。
- [x] 应用清单版本同步为 `1.10.13.0`。

## v1.10.14 实测对比记录器

- [x] “提醒与功耗”页新增开始、停止和打开目录操作。
- [x] 每次开始记录生成独立会话编号与 CSV 文件。
- [x] 记录目录位于运行程序旁的 `功耗实测对比记录`。
- [x] CSV 使用 UTF-8 BOM 并包含固定字段表头。
- [x] 采样周期为 250ms，约每秒四条样本。
- [x] 记录整机估算、来源、置信度、预计误差及 CPU/GPU/电池等对比字段。
- [x] 设置页显示记录时长、样本数、记录编号和当前整机估算值。
- [x] 每四个样本刷新一次文件，停止记录和程序退出时强制刷新关闭。
- [x] CSV 写入失败时自动停止，并在设置页显示错误。
- [x] QA x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] 实机记录超过 60 秒：`power_comparison_20260623-142811.csv` 共 511 条，持续 127.718 秒。
- [ ] 记录过程中结束程序，确认 CSV 尾部完整且可正常打开。
- [ ] 手机先同时拍摄记录编号与电力表，再拍电力表 30 秒，验证视频和 CSV 可对齐。

## v1.10.14 版本标识验证

- [x] 程序内版本标识为 `v1.10.14 (2026-06-23 14:13)`。
- [x] Windows `FileVersion` 为 `1.10.14.0`。
- [x] Windows `ProductVersion` 为 `v1.10.14 (2026-06-23 14:13)`。
- [x] 应用清单版本同步为 `1.10.14.0`。

## v1.10.15 CSV Unicode 路径修正

- [x] 确认 v1.10.14 在中文发布路径下创建了乱码目录。
- [x] 确认路径分隔符被编码破坏后，CSV 实际落在发布目录根部。
- [x] 程序路径改为 `GetModuleFileNameW` 获取。
- [x] 目录和文件拼接改为 `std::filesystem::path`。
- [x] 目录创建和检查使用 Unicode 路径及 `std::error_code`。
- [x] `std::ofstream` 直接使用 `std::filesystem::path` 打开 CSV。
- [x] “打开记录目录”改为 `ShellExecuteW`。
- [x] 记录目录固定为 `PowerComparisonRecords`。
- [x] 设置页路径通过 UTF-16 到 UTF-8 转换显示。
- [x] CSV 表头写入后立即检查文件流状态。
- [x] QA x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] 实机开始记录，确认 CSV 位于 `PowerComparisonRecords` 内。
- [x] 停止记录后确认 CSV 为完整 UTF-8 BOM 表格，可正常解析 511 条数据。

## v1.10.15 版本标识验证

- [x] 程序内版本标识为 `v1.10.15 (2026-06-23 14:22)`。
- [x] Windows `FileVersion` 为 `1.10.15.0`。
- [x] Windows `ProductVersion` 为 `v1.10.15 (2026-06-23 14:22)`。
- [x] 应用清单版本同步为 `1.10.15.0`。

## v1.10.16 实测记录刷新与可拟合字段

- [x] 核对 CSV 会话编号为 `20260623-142811`，开始时间为 `2026-06-23 14:28:11.580`。
- [x] CSV 持续 127.718 秒，视频持续 128.453 秒，时长差约 0.735 秒。
- [x] CSV 的 CPU Package 全程为 `60.139W`，独显全程为 `13.886W`，确认传感器缓存被冻结。
- [x] `system_power_w` 仅在 `130.693W–130.731W` 间变化，无法解释视频中的真实负载波动。
- [x] 视频每 4 秒抽样观察到电力表约在 `127.6W–160.3W` 间变化。
- [x] 未把该批时间未可靠配对的异常点加入校准拟合，原线性公式保持不变。
- [x] 设置中心打开且实测记录开启时，不再暂停 LHWM、外部功耗和 Windows 功耗计数器采集。
- [ ] 实机结果表明该实现仍被同步传感器调用阻塞，实际平均间隔约 2049ms；由 v1.10.17 继续修复。
- [x] 记录开始后的首轮强制刷新 CPU/GPU 采集时间点。
- [x] CSV 新增 `raw_system_power_w`、`fusion_internal_w`、`fusion_outlet_calibrated_w`。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [ ] Windows 界面自动检查组件缺少内部模块，待组件恢复后抓取 QA 窗口并复核七页布局。
- [ ] 使用 v1.10.16 再录制一组同时包含记录编号/开始画面与电力表的视频，确认 CPU/GPU 和三种功耗字段持续变化。

## v1.10.16 版本标识验证

- [x] 程序内版本标识为 `v1.10.16 (2026-06-23 14:35)`。
- [x] Windows `FileVersion` 为 `1.10.16.0`。
- [x] Windows `ProductVersion` 为 `v1.10.16 (2026-06-23 14:35)`。
- [x] 应用清单版本同步为 `1.10.16.0`。

## v1.10.17 250ms 非阻塞记录修复

- [x] 分析 `power_comparison_20260623-144638.csv`：90.125 秒共 44 条，平均间隔 2049.419ms。
- [x] 确认 CPU Package、独显、原始功耗和最终功耗均真实变化，数值冻结问题已经解决。
- [x] 确认 v1.10.16 的失败原因是主线程同步执行整套 LHWM/ASUS WMI 等耗时采集。
- [x] 记录期间不再在主线程调用整套 `PollLHWMStats`、外部功耗计数器和频率采集。
- [x] 核心功耗传感器改为后台快速采集，且同一时刻最多存在一个采集任务。
- [x] 后台结果以完整快照形式发布，主线程只应用已完成的快照。
- [x] CSV 新增 `sensor_sample_sequence` 和 `sensor_sample_age_ms`。
- [x] 停止记录后恢复原有完整传感器采集行为。
- [x] 程序退出时等待正在运行的后台采集任务结束。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [ ] 实机记录 60 秒，确认约生成 240 条样本，平均间隔接近 250ms。
- [ ] 检查 `sensor_sample_sequence` 和 `sensor_sample_age_ms`，确认慢传感器产生的重复快照可被识别。
- [ ] 配合电力表视频复测稳定负载与场景切换。

## v1.10.17 版本标识验证

- [x] 程序内版本标识为 `v1.10.17 (2026-06-23 15:56)`。
- [x] Windows `FileVersion` 为 `1.10.17.0`。
- [x] Windows `ProductVersion` 为 `v1.10.17 (2026-06-23 15:56)`。
- [x] 应用清单版本同步为 `1.10.17.0`。

## v1.10.18 视频对齐与新校准

- [x] CSV 会话编号为 `20260623-161055`，共 713 条，持续 178.25 秒。
- [x] CSV 间隔最小 250ms、最大 266ms，全部满足连续时间轴要求。
- [x] 视频持续 177.656 秒，与 CSV 总长度差约 0.59 秒。
- [x] 排除新会话开始前沿用的 5 条陈旧样本，不参与分析。
- [x] 通过负载爬升交叉对齐，最佳传感器补偿约为 1.5 秒。
- [x] 低负载稳态区间选取视频约 16–54 秒。
- [x] 高负载稳态区间选取视频约 76–168 秒。
- [x] 排除 56–74 秒加载爬升、场景切换和结束下降瞬态。
- [x] 旧公式低负载平均偏差约 `+10.5W`，高负载平均偏差约 `-1.4W`。
- [x] 新公式为 `1.0218875 × 原融合估算 + 25.8991W`。
- [x] 新公式低负载 MAE 约 `4.4W`，高负载 MAE 约 `5.2W`，总体约 `4.9W`。
- [x] 当前数据选择新直线，不增加缺乏证据的分段曲线。
- [x] 每次开始记录时重置后台传感器样本时间戳和序号。
- [x] 首个新传感器快照完成前，CSV 不写入陈旧估算及 CPU/GPU 数值。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [ ] 使用 v1.10.18 复测低、中、高三个稳定负载区间。

## v1.10.18 版本标识验证

- [x] 程序内版本标识为 `v1.10.18 (2026-06-23 16:26)`。
- [x] Windows `FileVersion` 为 `1.10.18.0`。
- [x] Windows `ProductVersion` 为 `v1.10.18 (2026-06-23 16:26)`。
- [x] 应用清单版本同步为 `1.10.18.0`。

## v1.10.25 窗口化全屏显示路径修复

- [x] 修改前归档 v1.10.24 到 `旧版本归档\2026-06-29_2351_v1.10.24_窗口化全屏显示修复前`。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] Windows `FileVersion` 为 `1.10.25.0`。
- [x] Windows `ProductVersion` 为 `v1.10.25 (2026-06-29 23:56)`。
- [x] 应用清单版本同步为 `1.10.25.0`。
- [x] 当前发布目录已同步 `overlay.exe` 和 `overlay_v1.10.25_20260629-2356.exe`。
- [x] 静态检查确认游戏显示状态下 Overlay 宿主窗口使用游戏显示器 `rcMonitor`，桌面状态回到工作区。
- [x] 静态检查确认已识别游戏目标会重申 `HWND_TOPMOST`，普通桌面应用全屏/最大化仍会清掉游戏显示状态。
- [ ] 用户实机验证三角洲行动窗口化全屏/无边框全屏是否显示 Overlay。
- [ ] 用户实机验证普通应用全屏/最大化仍不监测并隐藏。
- [ ] 若日志显示 `gameDisplay=1` 且 `Overlay host updated: gameMonitor=1`，但画面仍完全不可见，应按外部 Win32 Overlay 被游戏/ACE 层级压制处理，下一步评估 Xbox Game Bar Widget 等非注入显示通道。
<!-- 2026-06-28 v1.10.20 QA: Release and FPSOVERLAY_UI_QA builds passed. Verified release config points to NVIDIA selectedGpu=1 and gpuCoreFreqPath=/gpu-nvidia/0/clock/0. Formal release app was not launched. -->
<!-- 2026-06-28 v1.10.21 QA: Release build passed; FPSOVERLAY_UI_QA build passed. Verified current release contains overlay.exe and overlay_v1.10.21_20260628-1925.exe. Formal release app was not launched. -->
<!-- 2026-06-29 v1.10.22 QA: Release and FPSOVERLAY_UI_QA builds passed. Static log review showed prior windowed-fullscreen sessions flipping target PID to launcher/helper apps; new code excludes known desktop helpers from target selection and uses strong-game ETW fallback. Formal release app was not launched. -->
<!-- 2026-06-29 v1.10.23 QA: Release and FPSOVERLAY_UI_QA builds passed. Static log review confirmed DeltaForceClient-Win64-Shipping.exe was detected but v1.10.22 cleared the target after window-state jitter; v1.10.23 keeps strong ETW game target active. Formal release app was not launched by Codex; existing elevated overlay process prevented replacing overlay.exe until user exits it. -->
<!-- 2026-06-29 v1.10.24 QA: Release and FPSOVERLAY_UI_QA builds passed. Static log review of v1.10.23 showed target=46464 was retained at 23:34:32 but cleared at 23:34:39 after protected-process checks. v1.10.24 treats OpenProcess ERROR_ACCESS_DENIED as alive and falls back to Toolhelp snapshots for normal PID-exit detection. Published overlay.exe and overlay_v1.10.24_20260629-2339.exe were synchronized; formal release app was not launched by Codex. -->
<!-- 2026-06-29 v1.10.25 QA: Release and FPSOVERLAY_UI_QA builds passed. Static review verifies active strong game targets drive a game overlay display state, protected games can match by exe name without full path access, and visible game state updates the transparent host window to the game monitor rcMonitor with periodic HWND_TOPMOST reassertion. Published overlay.exe and overlay_v1.10.25_20260629-2356.exe were synchronized; formal release app was not launched by Codex. -->

## v1.10.26 游戏 Owned Popup 显示路径

- [x] 修改前归档 v1.10.25 到 `旧版本归档\2026-06-30_1305_v1.10.25_游戏OwnedPopup显示修复前`。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] Windows `FileVersion` 为 `1.10.26.0`。
- [x] Windows `ProductVersion` 为 `v1.10.26 (2026-06-30 13:07)`。
- [x] 应用清单版本同步为 `1.10.26.0`。
- [x] 当前发布目录已同步 `overlay.exe` 和 `overlay_v1.10.26_20260630-1307.exe`。
- [x] 静态检查确认游戏态会查找目标进程最大可见顶层窗口并设置为 Overlay popup owner。
- [x] 静态检查确认普通桌面应用全屏/最大化、离开游戏目标和打开实时设置时会解除 owner 绑定。
- [x] 静态检查确认游戏态 `HWND_TOPMOST` 重申频率提高到可见游戏帧级别，桌面态仍保持低频。
- [ ] 用户实机验证三角洲行动窗口化全屏/无边框全屏是否显示 Overlay。
- [ ] 用户实机验证普通应用全屏/最大化仍不监测并隐藏。
- [ ] 若日志出现 `Overlay owner updated: owner=... gameMonitor=1` 且 `Overlay host updated: gameMonitor=1 ... owner=...`，但画面仍完全不可见，应按游戏/ACE 压制外部透明 Win32 Overlay 处理，继续评估 Xbox Game Bar Widget 等非注入显示通道。
<!-- 2026-06-30 v1.10.26 QA: Release and FPSOVERLAY_UI_QA builds passed. Added owned-popup path for confirmed game targets, per-frame topmost reassertion in visible game state, and owner detach on desktop fullscreen hiding/settings. Published overlay.exe and overlay_v1.10.26_20260630-1307.exe were synchronized; formal release app was not launched by Codex. -->

## v1.10.28 休眠唤醒窗口恢复

- [x] 修改前归档 v1.10.26 到 `旧版本归档\2026-07-09_1954_v1.10.26_休眠唤醒窗口修复前`。
- [x] 完整遍历发布日志，确认旧版累计出现 `452747` 条 `Overlay owner update failed`。
- [x] 日志确认外部 owner 成功绑定后，owner 窗口消失会紧接着出现 `err=1400`，说明 Overlay 主窗口被连带销毁。
- [x] 停用跨进程 owner 绑定，保留游戏显示器 `rcMonitor` 和 `HWND_TOPMOST` 路径。
- [x] 增加 `WM_POWERBROADCAST` 休眠前清理及唤醒后窗口样式恢复。
- [x] 增加 Overlay 主窗口意外销毁后的 DirectX/ImGui 宿主重建。
- [x] 增加托盘图标在电源恢复和 `TaskbarCreated` 后重新发布。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] QA 合成发送休眠与自动唤醒消息后，Overlay 主窗口和托盘窗口均继续存活。
- [x] QA 唤醒后窗口样式确认：`ToolWindow=true`、`AppWindow=false`、`NoActivate=true`、`owner=null`。
- [x] QA 在手动隐藏 Overlay 后合成休眠/唤醒，唤醒后仍保持隐藏，没有覆盖用户显示/隐藏意图。
- [x] QA 通过托盘退出命令正常结束，无残留 QA 进程。
- [x] Windows `FileVersion` 为 `1.10.28.0`。
- [x] Windows `ProductVersion` 为 `v1.10.28 (2026-07-09 19:57)`。
- [x] 应用清单版本同步为 `1.10.28.0`。
- [ ] 用户实机执行一次睡眠或休眠后唤醒，确认任务栏不出现不可点击的 FPS Overlay 图标。
- [ ] 用户实机确认唤醒后托盘菜单、设置和 Overlay 显示均可正常使用。
- [ ] 用户实机复测三角洲行动窗口化全屏/无边框全屏显示。
- [ ] 用户实机复测普通应用全屏/最大化仍按“仅桌面显示”规则隐藏。

## v1.10.29 游戏目标与低 FPS 计时修复

- [x] 修改前归档 v1.10.28 到 `旧版本归档\2026-07-10_0916_v1.10.28_游戏识别与低帧提醒修复前`。
- [x] 发布程序 v1.10.28 实机日志确认 `wallpaper64.exe` 被选为 ETW 游戏目标，并将 Overlay 宿主切换到完整显示器。
- [x] 静态检查确认此前“最近游戏”刷新会在活动游戏位于后台时写入前台桌面进程 PID。
- [x] 静态检查确认旧低 FPS 提醒按游戏帧时间累加，但在 Overlay 刷新循环中执行，持续时间与真实秒数不一致。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过；输出文件名由 `src/version.h` 自动生成。
- [x] QA 实机 ETW 会话确认：Wallpaper Engine 和 ChatGPT/Codex 都未成为目标，`gameMonitor=0`、无 owner 更新失败。
- [x] 低 FPS 提醒改为 `GetTickCount64` 实际经过时间计时，并在超过一秒的暂停后重置。
- [x] Windows `FileVersion` 为 `1.10.29.0`。
- [x] Windows `ProductVersion` 为 `v1.10.29 (2026-07-10 09:22)`。
- [x] 应用清单版本同步为 `1.10.29.0`。
- [ ] 用户实机启动游戏后确认真实游戏仍能被识别并显示 FPS。
- [ ] 用户启用低 FPS 提醒后，以固定阈值验证提醒在设定秒数附近触发。

## v1.10.30 休眠唤醒 NVIDIA 传感器崩溃修复

- [x] 修改前归档 v1.10.29 到 `旧版本归档\2026-07-10_2039_v1.10.29_休眠唤醒NVML崩溃修复前`，保留正式程序、源码包、配置、日志和最新诊断。
- [x] Windows 事件查看器确认 v1.10.29 的 `overlay.exe` 在 `nvml.dll` 内以 `0xc0000005` 访问冲突退出；故障时间为 `2026-07-10 18:13:42`。
- [x] 静态检查确认旧代码允许后台 LHWM 快照线程与主线程时钟/设置读取并发调用 NVIDIA 传感器库，且休眠唤醒时没有传感器读取冷却期。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 通过，发布 `overlay.exe` 的 Windows `FileVersion` 为 `1.10.30.0`。
- [x] `FPSOVERLAY_UI_QA` x64 资源编译、C++ 编译、链接和 LTCG 通过；设置诊断进程成功启动并完成五秒 UI 冒烟检查。
- [x] 静态检查确认除串行入口外不存在直接 `LHWM::GetSensorValue` 调用；发布包包含 `v1.10.30` 文档、源代码和 LHWM 运行库。
- [ ] 用户实机让程序运行后执行一次睡眠或休眠，并在唤醒后至少等待 20 秒；确认进程仍存在、托盘可点击、设置可打开。
- [x] v1.10.30 实机日志依次出现 `LibreHardwareMonitor polling paused for power suspend`、`delayed 15000 ms after power resume` 和 `polling resumed after power recovery`。
- [x] v1.10.30 在 `2026-07-10 21:36` 休眠并于 `2026-07-11 13:03` 唤醒后继续刷新功耗与传感器数据，Windows 没有新增 v1.10.30 `overlay.exe` 崩溃事件。
- [ ] 用户实机确认 Overlay 的游戏加加横向布局、指标顺序和“仅桌面显示”行为未变化。
- [ ] 用户实机确认开启“随 Windows 登录自动启动”后，计划任务 `FPS Overlay` 的实际程序路径为当前发布目录 `overlay.exe`，而不是旧版本归档。

## v1.10.31 代码审查问题修复与自审

- [x] 修改前归档 v1.10.30 到 `旧版本归档\2026-07-11_1337_v1.10.30_代码审查问题修复前`，保留正式程序、源码包、配置、日志和诊断文件。
- [x] LHWM 初始化、普通快照和功耗对比线程均为可等待线程；退出流程在释放 WMI 前逐一 `join`，不存在传感器 `detach()` 生命周期遗漏。
- [x] 所有传感器线程在自身线程执行 `CoInitializeEx(COINIT_MULTITHREADED)` 并配对 `CoUninitialize`。
- [x] `GetSensorValue` 与传感器映射初始化增加窄范围原生异常边界；异常后设置 LHWM 隔离状态，不继续调用故障供应商库。
- [x] 休眠窗口消息不再阻塞等待 LHWM 互斥量；手动重扫由主循环排队，并遵守唤醒冷却和在途采样状态。
- [x] LHWM 路径、GPU 列表和频率选项的后台发布与 UI/诊断读取使用状态锁或原子发布顺序。
- [x] DX11 `Present`、`ResizeBuffers`、`GetBuffer` 和渲染目标创建均检查返回值；设备或宿主恢复失败进入重试，不再令主循环退出。
- [x] Windows 电池设备查询与功耗 CSV 刷新均从 Overlay 主线程移出，工作线程在 Feature 关闭时完整回收。
- [x] 游戏自动悬浮窗改为进入/离开游戏边沿触发，只恢复由该功能改变的显示状态。
- [x] 配置保存使用同目录临时文件和 `MOVEFILE_WRITE_THROUGH` 原子替换；日志达到 16 MiB 后保留 previous 文件。
- [x] 便携 Release/QA 构建启用 `/W4 /permissive- /Zi`，并生成私有 PDB；第三方缺失 PDB 警告被定向忽略。
- [x] 编译器和链接器 PDB 均定向到 `build` 子目录；源码根目录无 `vc140.pdb`，源码包与 v1.10.30 条目范围一致。
- [x] 最终 Release x64 资源编译、C++ 编译、链接和 LTCG 无警告通过。
- [x] 最终 `FPSOVERLAY_UI_QA` x64 编译无警告通过；按本次 QA 进程 PID 合成发送挂起与自动恢复消息，消息均返回，17 秒后进程存活且响应，并通过托盘退出命令正常结束。
- [x] QA 日志在恢复后先记录 15000ms 冷却，再记录 `polling resumed after power recovery`；本次运行没有 `Atomic config save failed`，最近一小时没有 Overlay 相关 WER/Application Error。
- [x] Windows `FileVersion` 为 `1.10.31.0`，`ProductVersion` 为 `v1.10.31 (2026-07-11 13:55)`，应用清单同步为 `1.10.31.0`。
- [x] 与 v1.10.30 归档源码逐行比较，游戏加加 `drawSeg` 及指标顺序所在绘制段 207 行完全一致；七页设置导航仍为 `01`–`07`。
- [ ] 用户实机复测一次休眠/唤醒、一次显卡驱动重置场景以及游戏加加横向布局。

## v1.10.32 平均 FPS、数据新鲜度与托盘重扫

- [x] 修改前归档 v1.10.31 到 `旧版本归档\2026-07-12_1245_v1.10.31_第一批实用功能前`。
- [x] 静态检查确认平均 FPS 最多每秒采样一次，只使用 60 个固定内存样本。
- [x] 静态检查确认目标 PID 变更或清空时平均 FPS 重置，低 FPS 提醒仍接收实时 FPS。
- [x] 静态检查确认 LHWM 快照成功后发布样本时间戳，超时阈值不低于 5 秒。
- [x] 静态检查确认托盘菜单包含“重新扫描传感器”，命令通过 `WM_APP_TRAY_COMMAND` 交给主线程。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 无警告通过。
- [x] `FPSOVERLAY_UI_QA` x64 编译无警告通过；QA 进程启动 5 秒后存活且响应。
- [x] Windows `FileVersion` 为 `1.10.32.0`，`ProductVersion` 为 `v1.10.32 (2026-07-12 12:49)`，Release/QA manifest 均为 `1.10.32.0`。
- [x] 与 v1.10.31 归档比较游戏加加绘制块 236 行；仅 FPS 读取变为 `displayFps`，分段数量、指标顺序和绘制样式未变。
- [ ] 用户实机启动游戏，确认“使用最近 60 秒平均 FPS”开关开启后数值平滑变化。
- [ ] 用户实机切换游戏或退出游戏后重新进入，确认平均 FPS 不沿用上一个目标的数据。
- [ ] 休眠唤醒或暂停硬件采样超时后，确认设置状态轨显示“数据延迟”且不自动隐藏 Overlay 指标。
- [ ] 用户实机点击托盘“重新扫描传感器”，确认界面不卡顿、日志出现重扫请求和初始化结果。
- [ ] 用户实机确认游戏加加横向布局、指标顺序、分段样式和“仅桌面显示”行为未变。

## v1.10.33 游戏峰值、FPS 颜色阈值与电池状态

- [x] 修改前归档 v1.10.32 到 `旧版本归档\2026-07-12_1343_v1.10.32_第二批实用功能前`。
- [x] 静态检查确认游戏峰值只保留 CPU/GPU 占用、温度和功耗的最大值，没有文件 IO。
- [x] 静态检查确认目标 PID 变更、游戏结束或关闭功能会清零峰值。
- [x] 静态检查确认自定义 FPS 颜色默认关闭，阈值保持 `15 <= 红色上限 < 绿色起点 <= 240`。
- [x] 静态检查确认电池百分比来自 `GetSystemPowerStatus`，`255` 未知值和无电池状态不会当作有效电量。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 无警告通过。
- [x] `FPSOVERLAY_UI_QA` x64 编译无警告通过；QA 进程启动 5 秒后存活且响应。
- [x] Windows `FileVersion` 为 `1.10.33.0`，`ProductVersion` 为 `v1.10.33 (2026-07-12 13:47)`，Release/QA manifest 均为 `1.10.33.0`。
- [x] 与 v1.10.32 归档比较游戏加加主绘制块；仅 FPS 颜色选择增加两行，原有指标顺序和 `drawSeg()` 样式未变。
- [ ] 用户实机进入游戏后确认 CPU/GPU 占用、温度和功耗峰值只增不减。
- [ ] 用户实机切换游戏或退出游戏，确认峰值清零。
- [ ] 开启自定义 FPS 颜色，分别使用低于、介于和高于阈值的 FPS 验证红/黄/绿颜色。
- [ ] 关闭自定义 FPS 颜色，确认各布局恢复 v1.10.32 的默认颜色规则。
- [ ] 在笔记本上开启“电池电量与供电状态”，确认插电、充电和电池供电文字正确。
- [ ] 关闭电池状态开关时，确认游戏加加分段数量、指标顺序和绘制样式与 v1.10.32 一致。

## v1.10.34 布局快捷键、诊断摘要与恢复通知

- [x] 修改前归档 v1.10.33 到 `旧版本归档\2026-07-12_1355_v1.10.33_第三批实用功能前`。
- [x] 静态检查确认布局快捷键只循环现有四种布局，不新增布局或改写指标顺序。
- [x] 静态检查确认诊断摘要只读取当前内存快照并写入 `CF_UNICODETEXT`，不包含文件 IO 或传感器调用。
- [x] 静态检查确认恢复状态不清除 `kLhwmResumeCooldownMs=15000`，只在新快照应用后标记恢复。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 无警告通过。
- [x] `FPSOVERLAY_UI_QA` x64 编译无警告通过。
- [x] Windows `FileVersion` 为 `1.10.34.0`，`ProductVersion` 为 `v1.10.34 (2026-07-12 13:59)`，Release/QA manifest 均为 `1.10.34.0`。
- [x] QA 合成发送 `PBT_APMRESUMEAUTOMATIC` 成功，17 秒后进程存活且响应，日志保留 15000ms 冷却和恢复状态。
- [x] 与 v1.10.33 归档比较游戏加加主绘制块：均为 238 行，内容零差异。
- [ ] 用户实机按默认 `F9`，确认四种布局按顺序循环，且重启后保留最后的布局。
- [ ] 修改布局快捷键，确认 ESC 可取消录制且新快捷键生效。
- [ ] 在设置和托盘分别点击“复制诊断摘要”，确认剪贴板为简体中文且包含当前版本和目标状态。
- [ ] 实机休眠/唤醒后确认先出现“正在恢复监测”，并在新传感器快照后出现“监测已恢复”。
- [ ] 人为使传感器超过 45 秒无新快照，确认提示恢复超时且托盘重扫可重新进入恢复状态。
- [ ] 确认游戏加加布局、指标顺序、绘制样式和“仅桌面显示”行为与 v1.10.33 一致。

## v1.10.35 游戏目标、托盘恢复与诊断文本优化

- [x] 修改前归档 v1.10.34 到 `旧版本归档\2026-07-12_1823_v1.10.34_再次代码审查优化前`。
- [x] 静态检查确认未知前台进程不会直接写入 `g_targetPid`，目标只在最终稳定判定后统一发布。
- [x] 静态检查确认当前目标失去证据时最多保留 3 秒，新目标切换需要稳定 1.5 秒。
- [x] 静态检查确认普通应用全屏/最大化仍可立即阻止游戏目标兜底。
- [x] 静态检查确认托盘恢复先尝试修改，失败后按 1000/3000/8000ms 重试，并继续监听 `TaskbarCreated`。
- [x] 静态检查确认传感器诊断使用 Unicode 路径、UTF-8 BOM，并在写入前移除内嵌 NUL。
- [x] 静态检查确认 `WinHTTP`、旧上游仓库 URL 和启动更新检查代码已移除。
- [x] Release x64 资源编译、C++ 编译、链接和 LTCG 无警告通过。
- [x] `FPSOVERLAY_UI_QA` x64 编译无警告通过。
- [x] Windows 文件版本、产品版本和 Release/QA manifest 同步为 v1.10.35。
- [x] 与 v1.10.34 归档比较游戏加加主绘制块内容零差异，双方 SHA256 均为 `5BD6BB6A6901FEA9DB0AB16AA87DF6E18E012858F83765CA9907D94467DA8EC2`。
- [x] QA 合成休眠/唤醒后 4 个窗口保持存活和响应，托盘恢复成功日志仅 1 条，15 秒硬件冷却后恢复轮询。
- [ ] 用户实机验证《鸣潮》主进程不会再与 `KRSDKExternal.exe` / `KRWebView.exe` 反复抢目标。
- [ ] 用户实机休眠/唤醒后确认托盘图标存在且菜单可打开。
- [ ] 实机重扫传感器后确认新 `sensor-diagnostics.txt` 可作为普通 UTF-8 文本打开且不含 NUL。
