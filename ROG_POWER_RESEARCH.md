# ROG G815LR 功耗接口调查

调查日期：2026-06-18

## 开源项目结论

- G-Helper
  - ASUS ACPI `DSTS` 支持风扇、温度、充电类型、功率限制等设备 ID。
  - `0x0012005A` 是部分机型的电池放电功率，不是适配器 DC 输入。
  - 普通笔记本的充放电功率仍来自 Windows Battery API。
- Linux `asus-wmi` / `asusctl`
  - 公开了充电类型、充电上限、CPU/GPU/平台功率限制。
  - 没有公开 G815LR 的适配器输入功率或整机实时功耗设备 ID。
- LibreHardwareMonitor
  - 可读 CPU Package、GPU Package 和电池充放电功率。
  - 当前 G815LR 没有暴露 AC Adapter、DC Input 或 Total System Power 传感器。

## 本机实测

- 机型：ROG Strix G18 G815LR。
- LibreHardwareMonitor：7 个 Power 传感器，均为 CPU/GPU 组件功耗。
- Windows Power Meter：实例存在，但 `Power` 当前恒为 0。
- Windows Energy Meter：
  - `RAPL_Package0_PKG` 可实时返回 CPU 包功耗。
  - 性能计数器值按毫瓦换算为瓦。
- ASUS ATKACPI：已知设备 ID 中没有发现可验证的 DC 输入瓦数。

## 当前实现

真实值优先级：

1. AIDA64 共享内存中的 `PDCIN` / `DC Input`：作为 DC 输入功耗。
2. LibreHardwareMonitor 的 AC Adapter / Input Power：作为 DC 输入功耗。
3. Windows Power Meter：有效时作为整机实测功耗。
4. Windows Energy Meter `RAPL_Package0_PKG`：作为实时 CPU 包功耗来源。
5. 没有整机硬件表计时，使用实时 CPU/GPU 功耗加平台开销生成明确带 `≈` 的估算。

DC 输入不存在真实来源时显示 `N/A`，不会用估算值冒充。

## 参考源码

- https://github.com/seerge/g-helper/blob/main/app/AsusACPI.cs
- https://github.com/seerge/g-helper/blob/main/app/HardwareControl.cs
- https://github.com/torvalds/linux/blob/master/include/linux/platform_data/x86/asus-wmi.h
- https://github.com/torvalds/linux/blob/master/drivers/platform/x86/asus-wmi.c
- https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitorLib/Hardware/Battery/Battery.cs
- https://learn.microsoft.com/windows-hardware/drivers/powermeter/overview-of-the-power-metering-and-budgeting-infrastructure
- https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhcollectquerydata
