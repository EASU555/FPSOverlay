# FPSOverlay 接续开发交接

更新时间：2026-07-16

## 当前交付状态

- 当前版本：`v1.10.38`
- 编译时间标识：`20260716-1451`
- 项目根目录：`C:\Users\ROG\Desktop\FPSOverlay_完整项目`
- 当前源码：`C:\Users\ROG\Desktop\FPSOverlay_完整项目\当前版本\源代码`
- 当前发布程序：`C:\Users\ROG\Desktop\FPSOverlay_完整项目\当前版本\发布程序`
- 当前源码包：`C:\Users\ROG\Desktop\FPSOverlay_完整项目\当前版本\源代码.zip`
- 旧版本归档：`C:\Users\ROG\Desktop\FPSOverlay_完整项目\旧版本归档`
- 当前发布文件：
  - `overlay.exe`
  - `overlay_v1.10.31_20260711-1355.exe`
  - `overlay_v1.10.32_20260712-1249.exe`
  - `overlay_v1.10.33_20260712-1347.exe`
  - `overlay_v1.10.34_20260712-1359.exe`
  - `overlay_v1.10.35_20260712-1828.exe`
  - `overlay_v1.10.36_20260714-2335.exe`
  - `overlay_v1.10.37_20260715-1911.exe`
  - `overlay_v1.10.38_20260716-1451.exe`
- 当前 `overlay.exe` SHA256：`4C501D945173F1CF855F82EC1ECF119F72719E5CA251F1EC22B3AA5428835795`
- 最近归档：`旧版本归档\2026-07-16_1439_v1.10.37_游戏性能报告前`

## 新会话开始前必须检查

按顺序阅读并核对：

1. `.codexignore`
2. `HANDOFF.md`
3. `FEATURES.md`
4. `TESTING.md`
5. `src\`
6. `features\`
7. `FPSOverlay.vcxproj`
8. 发布目录中的最新日志和传感器诊断数据

不要从头重做。先确认当前实现、日志和真实传感器数据，再决定修改内容。

## 必须保留的现有能力

- AIDA64 共享内存功耗读取。
- 传感器融合整机功耗估算。
- 电池充放电及插电偷电监控。
- 电池辅助供电提醒。
- 整机、已知组件、电池放电独立显示开关。
- “仅游戏中显示功耗项”及非游戏状态显示选择。
- 托盘“功耗显示”快捷子菜单。
- 网络下载、上传速度独立开关。
- 显存与内存独立显示。
- 独立托盘线程、快速打开菜单、设置窗口居中。
- Windows 开机自启动开关，使用任务计划程序实现。
- 设置窗口自由缩放和当前年轻科技风分区界面。

## 当前界面状态

`v1.10.29` 继承现有 UI 与显示逻辑，并保留 v1.10.18 的 G815LR 插座校准：

- 石墨深蓝底色，亮蓝、紫色和薄荷绿作为状态强调色。
- 编号侧边导航、品牌头部和首页状态栏。
- 首页各列独立紧凑排列，已消除大块无效空白。
- 卡片层级、细边框和页面编号统一。
- 小窗口下保留紧凑品牌与导航。
- 滑块标签移至滑块上方，避免裁切。
- 已启用并验证键盘 Tab 焦点导航。
- 托盘菜单弹出前显式激活独立托盘 owner 窗口，点击菜单外部且不选择命令时可由 Windows 正常关闭。
- 侧边栏新增编号 `08` 的“游戏报告”页面，显示最近一次游戏性能记录、统计卡片、硬件环境和完整会话曲线。
- 游戏报告每秒读取现有缓存，不主动查询硬件；CSV 由后台线程保存到 `GameSessionReports`。
- 侧边栏新增“桌面显示”独立页面，集中提供“仅桌面显示监测”开关、当前状态和行为说明。
- 明确的普通应用最大化、铺满工作区或全屏时临时隐藏 Overlay。
- 游戏路径、可执行文件特征或前台持续帧渲染确认游戏后，Overlay 优先保持显示。
- 未知全屏程序先保持显示并等待识别，避免游戏启动和加载阶段被立即误隐藏。
- 自动隐藏期间前台 PID 与 ETW 帧检测继续运行，切换到游戏后可以自行恢复。
- 自动隐藏不修改用户手动显示/隐藏意图；设置面板打开时保持可见。
- 窗口化全屏 / 无边框全屏识别到游戏目标后，外部透明 Win32 Overlay 会切到游戏所在显示器的完整 `rcMonitor`，并周期性重申 `HWND_TOPMOST`。
- 受保护游戏即使只能读取 exe 名、读不到完整路径，也可继续走强游戏身份识别；ETW 强游戏目标会刷新游戏显示保持状态。
- Overlay 不再设置跨进程 owner，避免游戏退出、锁屏或休眠时由 Windows 连带销毁；游戏态仍使用完整显示器区域并提高置顶重申频率。
- 休眠前清理遗留 owner，唤醒后恢复工具窗口、穿透、不可激活和置顶样式，并重新发布托盘图标。
- Overlay 主窗口意外失效时会重建透明宿主窗口和 DirectX/ImGui 后端，不再保留无响应的后台进程。
- 普通浏览器、聊天、启动器、编辑器等明确桌面应用全屏或最大化时仍会清掉游戏显示保持状态并临时隐藏。
- Wallpaper Engine 和 ChatGPT/Codex 等桌面图形程序即使持续呈现或位于游戏库目录，也不会作为 ETW 游戏目标。
- 已确认游戏短暂后台运行时，仅刷新该游戏的保持时间，绝不把前台资源管理器、启动器或聊天程序写成“最近游戏”。
- 低 FPS 提醒使用真实经过时间累计；睡眠、唤醒或长时间界面暂停不会提前触发提醒。
- 诊断摘要新增 `Game overlay display`、目标 PID、显示原因、Overlay host rect 和 owner 句柄，用于区分目标识别失败与窗口层级压制。
- 控制中心状态卡片使用动态高度，高 DPI 下不再出现文字和边框重叠。
- 设置中心打开时使用普通窗口层级，不再始终覆盖其他应用；关闭后恢复 Overlay 置顶。
- 托盘图标菜单新增“仅桌面显示”勾选开关，并立即保存配置。
- G815LR 插电融合估算使用 `1.0218875 × 原估算 + 25.8991W` 转换为插座端功耗。
- 新曲线来自 250ms 连续记录与电力表视频，按传感器样本年龄补偿约 1.5 秒后，对低负载和高负载稳态簇共同拟合。
- 游戏加载、场景切换、退出以及明显时间错位样本不加入校准拟合。
- 真实输入、真实整机传感器和电池放电来源不使用插座校准。
- “提醒与功耗”页面可启动 250ms 周期的 CSV 功耗记录。
- CSV 保存整机估算、时间戳、来源、置信度、CPU/GPU、充放电、FPS 和游戏状态。
- 开始实测记录后，即使设置中心保持打开，CPU/GPU 等重传感器也会继续刷新，不再把开始记录前的缓存值重复写满 CSV。
- 功耗拟合所需的 CPU/GPU 功耗与 LHWM 风扇值改为后台快速采集，不再由主界面线程同步读取整套传感器。
- CSV 主时间轴保持每 250ms 写入，不会再被耗时约 2 秒的 LHWM/ASUS WMI 查询阻塞。
- CSV 新增 `raw_system_power_w`、`fusion_internal_w` 和 `fusion_outlet_calibrated_w`，用于分离模型原始误差、插座校准误差和平滑延迟。
- CSV 新增 `sensor_sample_sequence` 和 `sensor_sample_age_ms`；同一传感器样本被连续写入时可被明确识别，不会误当成多次新测量。
- 每次开始新记录都会清空上一会话的传感器时间戳；首个新快照完成前，估算与 CPU/GPU 字段留空。
- 记录文件位于发布程序旁的 `PowerComparisonRecords` 文件夹，便于与电力表视频一起交付分析。
- 路径创建、CSV 打开和目录打开均使用 Unicode 接口，中文项目路径不会再产生乱码。
- v1.10.14 已产生的乱码文件保留，不自动删除。

不要改变 Overlay 本体的既有视觉风格，尤其不得破坏“游戏加加”横向布局。此次设置界面美化没有修改 Overlay 配色和核心渲染布局。

## 下一阶段核心目标

1. 继续优化整机功耗估算模型，提高不同负载、插电和电池状态下的准确性。
2. 分析真实整机功耗、AIDA64 共享内存、Windows Battery、CPU/GPU 功耗及传感器融合之间的误差。
3. 优化模型校准、平滑、置信度和来源选择，避免跳变及错误估算。
4. 不设置人为功耗上限，保留无限制整机功耗模型。
5. 优化性能、稳定性、资源占用和交互延迟时，必须先诊断再修改。

建议先根据同一时间轴对齐以下数据：

- AIDA64 整机或相关功耗字段。
- Windows 电池充放电功率和交流电状态。
- CPU Package、GPU Board/Chip 功耗。
- 已知组件功耗和融合模型输出。
- 数据来源状态、采样间隔、置信度和来源切换事件。

重点检查来源切换时的突变、插电状态下电池辅助供电、低负载基线漂移、传感器失效回退和不同采样周期造成的相位误差。

## 修改和交付规则

- 遵守 `.codexignore`。
- 配置 key 尽量保持向后兼容。
- 不删除旧版本；交付前将上一版放入“旧版本归档”。
- 每次程序修改后执行 Release build。
- 未明确要求时不要运行或发布正式程序。
- 编译完成后把新版 `overlay.exe` 同步到“当前版本\发布程序”。
- 正式交付时更新“当前版本\源代码.zip”。
- 每次交付递增版本号，并在文件名中附带日期和时间。
- UI 自动检查使用 `FPSOVERLAY_UI_QA` 测试构建，避免初始化硬件监控和触发提权；正式 Release 不定义该宏。

## 最近验证结果

- `v1.10.40` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；QA 名称分类及既有报告自检运行通过并以代码 0 正常退出。
- v1.10.40 Release 输出为 `overlay_v1.10.40_20260716-1758.exe`；用户当前仍在运行 v1.10.39，发布目录根部 `overlay.exe` 需在该进程退出后替换。
- v1.10.39 第二轮实测确认：没有新增 `explorer.exe` 会话；《绝地潜兵 2》1,556 条样本中硬件与功耗有 1,555 条有效，功耗覆盖率 99.9%，报告自动打开时机正常。
- 第二轮实测发现 `GameGuard.des` 被误记为 6 秒会话；v1.10.40 将 GameGuard、GameMon、Easy Anti-Cheat、BattlEye 和常见受保护游戏引导程序加入现有非游戏排除表。
- v1.10.39 已归档到 `旧版本归档\2026-07-16_1757_v1.10.39_GameGuard误识别修复前`，归档保留五份用户实测 CSV。
- `v1.10.39` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Release 输出为 `overlay_v1.10.39_20260716-1651.exe`。
- v1.10.38 实测生成两份 `explorer.exe` 假会话；v1.10.39 通过 `FeatureContext` 的非游戏目标标记阻止已知桌面进程、系统 Shell 和程序自身启动报告会话。
- v1.10.38 实测真实《鸣潮》报告有 4,293 条样本，但硬件指标仅前 4 条有效；根因是报告自动打开后设置中心暂停了异步硬件轮询。v1.10.39 在报告候选/记录期间继续原有异步轮询，并拒绝将过期融合功耗计入覆盖率和能耗。
- 游戏 PID 直接切换时不再自动打开旧报告，避免同一 Overlay 窗口切到设置中心导致新游戏内监控消失；旧报告仍会完成和保存。
- QA 实际运行 5 秒保持响应，桌面目标排除、过期估算功耗和游戏切换不弹窗自检通过，并通过程序退出命令以代码 0 结束；v1.10.39 正式程序未启动。
- 修改前版本已归档到 `旧版本归档\2026-07-16_1648_v1.10.38_实机报告修复前`，其中保留三份用户实测 CSV。
- `v1.10.38` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Release 输出为 `overlay_v1.10.38_20260716-1451.exe`。
- 新增 `GameSessionReportFeature` 并替换旧游戏峰值 Feature；会话稳定 2 秒开始、PID 切换/进程退出/目标丢失 5 秒结束，更新顺序位于 `LaptopPowerFeature` 之后。
- QA 自动验证统计、N/A 排除、1% Low、梯形能耗、短局/30 秒自动打开阈值、PID 切换、上一局保留、关闭清 activeSession 和中文路径异步 CSV。
- 当前自动 QA 未启动正式发布程序；真游戏退出、休眠中退出和实际/估算功耗来源显示仍需用户按 `TESTING.md` 实机确认。
- 源码差异未触及游戏加加 `drawSeg()`、Steam、垂直或水平 Overlay 的指标顺序和绘制样式。
- `v1.10.37` Release 和 `FPSOVERLAY_UI_QA` 均编译通过；Release 输出为 `overlay_v1.10.37_20260715-1911.exe`。
- CPU 温度 WMI 查询改为后台工作线程，主线程只发出合并请求并应用结果；WMI 枚举单次等待上限为 500ms。
- CPU/GPU LHWM 频率读取已并入现有后台硬件快照，主线程不再直接进入 LHWM 读频调用；LHWM 不可用时仅保留快速的 Windows CPU 频率回退。
- 源码差异未触及游戏加加主绘制块、七个设置页面、指标顺序或绘制样式；正式 Release 未在交付过程中启动。
- `v1.10.36` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Windows `FileVersion` 为 `1.10.36.0`，`ProductVersion` 为 `v1.10.36 (2026-07-14 23:35)`。
- v1.10.36 托盘恢复改为持续退避重试，每次重新添加重建通知区数据，并以原子窗口句柄消除主线程/托盘线程的数据竞争。
- v1.10.36 增加单实例互斥；QA 双启动确认第二个实例以代码 0 退出，第一个实例保持运行并可干净退出。
- v1.10.36 前台识别每 250ms 只构造一次进程快照，游戏目标与桌面显示判定复用 PID、exe、路径和全屏结果。
- v1.10.36 配置保存与 LHWM 状态锁分离，临时文件先验证再替换，失败保留脏状态继续重试；诊断导出显存路径的数据竞争已修复。
- v1.10.36 未修改 Overlay 指标、七个设置页面、游戏加加绘制块、指标顺序和绘制样式；正式 Release 未在交付过程中启动。
- 2026-07-15 实机休眠唤醒同步：唤醒后由自启动机制启动 `v1.10.36`，日志记录 `App start`、LHWM 初始化成功，随后《鸣潮》游戏目标、Overlay 宿主和 FPS 数据均恢复正常。
- 本次实机结果证明“唤醒后自动启动并恢复监测”可用；由于休眠前没有运行同一份 `v1.10.36` 正式进程，尚不能证明“原进程存活并仅恢复托盘图标”。
- 休眠前日志中 v1.10.35 的 `Tray icon restore retries exhausted` 属于旧版本历史记录，不作为 v1.10.36 的验证结果。
- `v1.10.35` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Windows `FileVersion` 为 `1.10.35.0`，`ProductVersion` 为 `v1.10.35 (2026-07-12 18:28)`。
- v1.10.35 游戏目标改为最终稳定后统一发布：未知前台程序不能直接抢占，已确认目标失证据保留 3 秒，不同目标需稳定 1.5 秒再切换；`KRSDKExternal`、崩溃报告器和常见 CEF 子进程已排除。
- v1.10.35 托盘恢复优先修改现有图标，重新添加失败时按 1/3/8 秒重试；500ms 内重复的主窗口/托盘恢复消息会合并。
- v1.10.35 传感器诊断改为 Unicode 路径和 UTF-8 BOM，写入前清除 LHWM 字符串中的内嵌 NUL；未启用的旧上游 WinHTTP 更新检查代码已删除。
- v1.10.35 QA 合成休眠/唤醒后 4 个窗口全部响应，托盘只记录一次成功恢复，15000ms 冷却及随后恢复轮询均已确认。
- 与 v1.10.34 归档逐字符比较，游戏加加主绘制块均为 12827 字符，SHA256 均为 `5BD6BB6A6901FEA9DB0AB16AA87DF6E18E012858F83765CA9907D94467DA8EC2`。
- `v1.10.29` Release 和 `FPSOVERLAY_UI_QA` 均已成功编译。
- `v1.10.30` Release 和 `FPSOVERLAY_UI_QA` 均已成功编译；QA 设置界面进程启动五秒后正常关闭，正式发布程序未在交付过程中启动。
- `v1.10.31` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过，并生成私有 PDB。
- `v1.10.32` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Windows 文件版本、产品版本和两个 manifest 均已同步。
- `v1.10.33` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Windows `FileVersion` 为 `1.10.33.0`，`ProductVersion` 为 `v1.10.33 (2026-07-12 13:47)`。
- `v1.10.34` Release 和 `FPSOVERLAY_UI_QA` 均以 `/W4 /permissive- /Zi` 无警告编译通过；Windows `FileVersion` 为 `1.10.34.0`，`ProductVersion` 为 `v1.10.34 (2026-07-12 13:59)`。
- v1.10.34 新增默认 `F9` 布局循环快捷键，复用现有热键录制与原子保存。
- v1.10.34 设置和托盘新增“复制诊断摘要”，只将当前内存状态写入 Unicode 剪贴板，不扫描硬件也不写文件。
- v1.10.34 休眠唤醒恢复状态为正在恢复/已恢复/45 秒超时，通过托盘通知与设置状态轨显示；原 15 秒 LHWM/NVML 冷却保持不变。
- v1.10.34 QA 合成恢复消息正常返回，17 秒后进程仍存活且响应；日志依次记录恢复状态、15000ms 冷却和 `polling resumed after power recovery`。
- 与 v1.10.33 归档比较，游戏加加主绘制块均为 238 行且零差异；七个设置页、指标顺序和绘制样式未变。
- v1.10.33 新增内存中游戏峰值 Feature，只记录 CPU/GPU 最高占用、温度和功耗；目标切换、退出游戏或关闭功能时清零。
- v1.10.33 FPS 颜色阈值可选自定义，默认关闭；关闭时完全沿用 v1.10.32 的颜色逻辑。
- v1.10.33 电池百分比和充电/插电/电池供电状态来自 `GetSystemPowerStatus`；Overlay 开关默认关闭，开启后作为功耗扩展末尾项。
- 与 v1.10.32 归档对比，游戏加加主绘制块仅增加 FPS 颜色选择的两行；原有 235 行、指标顺序和 `drawSeg()` 样式不变。
- v1.10.33 QA 启动 5 秒后进程存活且响应；正式 Release 未在交付过程中启动。
- v1.10.32 平均 FPS 使用 60 个固定内存样本，每秒最多取一次；目标 PID 变更或清空时重置，低 FPS 提醒仍使用实时值。
- v1.10.32 LHWM 快照新增样本年龄，超过 `max(5000ms, 3×refreshMs)` 时设置状态轨显示“数据延迟”；不自动隐藏 Overlay 指标。
- v1.10.32 托盘新增“重新扫描传感器”，通过主线程排队现有 LHWM 后台初始化，并允许手动恢复已隔离的传感器会话。
- v1.10.32 QA 启动 5 秒后进程存活且响应；普通关闭请求按托盘常驻逻辑隐藏，本次随后仅强制结束 QA 测试进程，正式程序未启动。
- 与 v1.10.31 归档对比，游戏加加绘制块仍为 236 行，仅 FPS 数值来源从 `gameFps` 替换为 `displayFps`，分段、顺序和绘制样式未变。
- 便携脚本和 MSBuild 项目均显式把编译器 PDB 定向到 `build` 子目录；源码根目录不会再残留 `vc140.pdb`，源码包不包含 PDB 或其他构建目录产物。
- v1.10.31 QA 按本次进程 PID 合成发送挂起与自动恢复消息：窗口消息均正常返回，17 秒后进程仍存活且响应，15 秒冷却结束后日志出现 `polling resumed after power recovery`，并可通过托盘退出命令正常结束。
- v1.10.31 QA 配置退出保存没有再出现 `Atomic config save failed`；启动日志和 Windows 文件属性均为 `v1.10.31 (2026-07-11 13:55)` / `1.10.31.0`。
- 自审逐行比较 v1.10.30 与 v1.10.31 的游戏加加绘制段共 207 行，内容完全一致；七个设置页面编号仍为 `01`–`07`。
- 实机 v1.10.28 日志确认 Wallpaper Engine 因 Steam 库路径被误认为强游戏；v1.10.29 已在强游戏与 ETW 自动候选两层排除明确桌面程序。
- QA 实机 ETW 会话确认 Wallpaper Engine 和 ChatGPT/Codex 都不会成为 FPS 目标，且没有 owner 更新失败。
- 完整发布日志确认 v1.10.26 的跨进程 owner 会在 owner 窗口销毁后造成 Overlay HWND 失效，随后累计产生 `452747` 条 owner 更新失败。
- QA 合成休眠/唤醒验证后主窗口和托盘窗口均存活；主窗口保持 `ToolWindow`、无 `AppWindow`、无 owner，手动隐藏状态不会被唤醒覆盖，并可通过托盘命令正常退出。
- `power_comparison_20260623-161055.csv` 共 713 条、178.25 秒，所有间隔为 250–266ms，视频长度为 177.656 秒。
- 对齐后的最佳传感器时间补偿约 1.5 秒；稳态区间为视频约 16–54 秒和 76–168 秒。
- 旧公式低负载平均高估约 10.5W，高负载平均低估约 1.4W，总体稳态 MAE 约 6.8W。
- 新公式低负载 MAE 约 4.4W，高负载 MAE 约 5.2W，总体稳态 MAE 约 4.9W。
- v1.10.16 实机记录 `power_comparison_20260623-144638.csv` 的功耗数值已真实变化，但 90.125 秒仅写入 44 条，平均间隔 2049ms，证明同步传感器读取阻塞了 250ms 时间轴。
- v1.10.17 已将记录所需的核心功耗采集移到后台；即使后台样本尚未更新，CSV 仍按 250ms 写入，并通过样本序号和年龄标记数据新鲜度。
- `power_comparison_20260623-142811.csv` 共 511 条、127.718 秒；对应视频为 128.453 秒。
- 该 CSV 的 CPU Package 全程为 `60.139W`、独显全程为 `13.886W`，确认 v1.10.15 在设置中心记录时冻结了传感器输入。
- 视频中的电力表约在 `127.6W–160.3W` 波动，因此本轮没有把失真的时间配对点加入线性拟合，原 G815LR 校准公式保持不变。
- 七个设置页面、常用窗口尺寸和键盘焦点已完成代码与构建检查。
- Windows 界面自动检查组件缺少内部模块，本轮未能抓取 QA 窗口截图；未改用正式程序代替。
- 正式发布程序没有在交付过程中启动。
- `FEATURES.md` 和 `TESTING.md` 已更新到 `v1.10.36`。

`README.md` 在 `.codexignore` 中，因此本轮没有改写；以 `HANDOFF.md`、`FEATURES.md`、`TESTING.md` 和实际源码为准。
<!-- 2026-06-28 v1.10.20: Fixed multi-GPU sensor binding. Stale Intel gpuCoreFreqPath can no longer force the overlay to monitor an iGPU when a discrete GPU has richer temperature/VRAM/power telemetry. Current release config is set to selectedGpu=1 and gpuCoreFreqPath=/gpu-nvidia/0/clock/0. -->
<!-- 2026-06-28 v1.10.21: Fixed game stutter after monitoring starts by moving normal LHWM/NVIDIA sensor polling off the render/UI thread. Main loop now schedules async hardware polling and applies completed snapshots, preventing periodic one-second sensor-query stalls. v1.10.20 archived before the change. -->
<!-- 2026-06-29 v1.10.22: Fixed borderless/windowed-fullscreen game monitoring. Target PID selection no longer switches to known desktop/launcher helper processes such as Steam WebHelper, WeGame browser, Codex, or Clash; confirmed strong-game processes can be recovered from ETW auto target and kept briefly during foreground-window jitter. Ordinary fullscreen desktop apps still use desktop-only hiding. -->
<!-- 2026-06-29 v1.10.23: PresentMon-style target selection pass for Delta Force / borderless fullscreen. Strong game identity now explicitly includes DeltaForce executables and path markers. ETW auto target with strong game identity can override foreground helper windows unless the foreground is a known desktop app in fullscreen/maximized state. Recent confirmed game target is kept with rolling refresh to avoid borderless fullscreen foreground jitter clearing FPS. -->
<!-- 2026-06-29 v1.10.24: Protected-game target retention fix. Recent-game and target-alive checks no longer treat ERROR_ACCESS_DENIED from OpenProcess as process exit, preventing ACE/protected Delta Force processes from clearing the FPS target while ETW evidence is still active. Release and FPSOVERLAY_UI_QA builds passed; formal release app was not launched by Codex. -->
<!-- 2026-06-29 v1.10.25: Borderless/windowed-fullscreen display path fix. Confirmed game targets now drive a separate game-overlay display state, protected-game identity can match on exe name even when full paths are unavailable, and the transparent Win32 overlay switches from work-area coverage to the game monitor rcMonitor with periodic HWND_TOPMOST reassertion. Ordinary known desktop fullscreen/maximized apps still clear the game display state and hide in desktop-only mode. Release and FPSOVERLAY_UI_QA builds passed; formal release app was not launched by Codex. -->
<!-- 2026-06-30 v1.10.26: Added a non-injection owned-popup display path for borderless/windowed-fullscreen games. When a confirmed game target has a visible top-level window, the transparent overlay temporarily sets that game window as its popup owner, uses the game monitor rcMonitor, and reasserts HWND_TOPMOST every visible game frame. Owner is detached when leaving game state, when ordinary fullscreen apps hide monitoring, or when live settings open. Release and FPSOVERLAY_UI_QA builds passed; formal release app was not launched by Codex. -->
<!-- 2026-07-09 v1.10.28: Fixed the unresponsive taskbar icon after sleep/wake. Cross-process owner assignment was removed because Windows destroys owned windows with their external owner. Power-resume style/tray restoration and overlay-host recovery were added. Release and FPSOVERLAY_UI_QA builds passed; synthetic QA suspend/resume preserved both windows and the formal release app was not launched by Codex. -->
<!-- 2026-07-10 v1.10.29: Fixed false game targets from Wallpaper Engine and ChatGPT/Codex, preserved the actual game PID while desktop helpers take foreground, and changed low-FPS alert duration to real elapsed time. Release and FPSOVERLAY_UI_QA builds passed; formal release app was not launched by Codex. -->
<!-- 2026-07-10 v1.10.30: Fixed post-sleep overlay crashes recorded in NVIDIA nvml.dll. LibreHardwareMonitor/NVML calls are now serialized, suspended before sleep, and delayed for 15 seconds after resume so the GPU driver can recover. Auto-start reconciliation now verifies the task's real executable path instead of trusting only config.ini. -->
<!-- 2026-07-11 v1.10.31: Code-review hardening. LHWM workers are joinable and COM-initialized, native vendor faults are quarantined, resume rescans honor cooldown, DX11 device loss is detected and retried, battery and CSV I/O moved off the render thread, config saves are atomic, logs rotate at 16 MiB, and Release/QA retain private PDBs with warning level 4. Final Release/QA builds passed without warnings. PID-scoped synthetic suspend/resume QA remained responsive through the full 15-second sensor cooldown and exited cleanly; the formal release app was not launched by Codex. -->
<!-- 2026-07-12 v1.10.32: Added an optional rolling 60-second average FPS display in the existing FPS slot, LHWM sample-age tracking with a delayed-data status, and a tray command for background sensor rescanning. Game++ segment order/style remains unchanged. Release and UI-QA builds passed without warnings; the formal release app was not launched by Codex. -->
<!-- 2026-07-12 v1.10.33: Added in-memory per-game CPU/GPU peaks, optional configurable FPS color thresholds, and optional Windows battery percentage/power-state display. Game++ keeps its existing segment order and drawing style; defaults preserve the v1.10.32 appearance. Release and UI-QA builds passed without warnings; the formal release app was not launched by Codex. -->
<!-- 2026-07-12 v1.10.34: Added a configurable layout-cycle hotkey, in-memory diagnostic-summary clipboard copy from settings/tray, and sleep-resume recovery notifications driven by fresh LHWM snapshots with a 45-second timeout. The 15-second NVML cooldown and Game++ drawing block remain unchanged. Release and UI-QA builds passed; synthetic resume QA stayed responsive. -->
