## NADIR Station 2.1.1

Windows 10/11 x64 安装包：下载 `NADIR_Station_2.1.1_win10_x64_setup.exe`，双击安装，可直接覆盖安装 2.1.0。默认装到当前用户目录（不需要管理员权限），解码数据和日志写在程序目录下。

### 新功能 · New
- **手动采样率 · Manual sample rate**：*Sample rate* 下拉最后一项 *Manual…*，选中后可直接输入任意采样率（MS/s）。原来的预设都还在。RTL-SDR 超过 2.4 MS/s 会退回 2.400 MS/s（GK-2A HRIT 的 3.2 MS/s 除外），HackRF One 接受 2–20 MS/s。
  A *Manual…* entry in the Sample rate box takes any rate in MS/s; the presets stay.
- **手动带宽 · Manual RF bandwidth**：*RF bandwidth* 同样新增 *Manual…*，直接输入任意带宽（MHz）；PlutoSDR 的模拟滤波器范围是 0.2–56 MHz，超出时按边界处理。
  A *Manual…* entry in the RF bandwidth box takes any bandwidth in MHz.

### 改动 · Changed
- **RF bandwidth 默认 None**：每次启动都是 *None (no bandwidth limit)*，选卫星不再自动改它；2.000 / 1.800 / 1.000 / 5.200 MHz 随时可选。None 时 Pluto 把模拟滤波器开到与采样率相当，RTL-SDR 和 HackRF 用自动带宽。
  RF bandwidth starts on None on every launch and a satellite choice no longer changes it.
- **IQ 回放 Max 默认播完整文件**：Range 默认 *Full file*。
  IQ playback: Max plays the full file; Range defaults to Full file.

### 修复 · Fixed
- **PlutoSDR 增益过高无法启动**：增益超过 AD9361 在当前频率能接受的上限（1.7 GHz 附近约 71 dB）时，板子会拒绝设置，接收被中止。现在会自动降到板子接受的最高值，并在状态栏提示。欢迎反馈。
  A gain above what the AD9361 accepts at the tuned frequency no longer stops the Pluto from starting; it is stepped down to the highest accepted value and the status bar says so.

GK-2A LRIT 解密密钥不随程序分发 · GK-2A LRIT keys are not included.
