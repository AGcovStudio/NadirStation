## NADIR Station 2.1.0

Windows 10/11 x64 安装包：下载 `NADIR_Station_2.1.0_win10_x64_setup.exe`，双击安装。默认装到当前用户目录（不需要管理员权限），解码数据和日志写在程序目录下。

### 新功能 · New
- **解码频偏 · Decoder frequency shift**（#4）：Live 页 Tuning 里的 *Frequency shift*。接收机偏开下行频率调谐，再在软件里把信号挪回中心，DC 尖峰就落在信号旁边。Pluto、RTL-SDR、HackRF 都支持，录 IQ 时录的是挪回之后的信号。
  The SDR is tuned off the downlink and the signal is moved back in software, so the DC spike sits beside the signal. Works with Pluto, RTL-SDR and HackRF.
- **HackRF One**（#2）：输入源新增 HackRF One，运行时加载 `hackrf.dll` / `libhackrf.so.0`，支持设备序号和天线口供电。
  New HackRF One input; libhackrf is loaded at run time.
- **GNU/Linux 编译**（#1）：合入 #3 的 CMake 修补，README 增加 Linux 编译说明（实验性）。
  GNU/Linux build fixes from #3 and build instructions (experimental).

### 其他 · Other
- 解码门限实测见 `docs/THRESHOLDS.md`。
- HackRF 部分没有在实机上测试，欢迎反馈。HackRF support has not been tested on hardware yet; reports welcome.

GK-2A LRIT 解密密钥不随程序分发 · GK-2A LRIT keys are not included.
