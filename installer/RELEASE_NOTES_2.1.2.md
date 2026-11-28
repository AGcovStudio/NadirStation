## NADIR Station 2.1.2

Windows 10/11 x64 安装包：下载 `NADIR_Station_2.1.2_win10_x64_setup.exe`，双击安装，可直接覆盖安装旧版本。默认装到当前用户目录（不需要管理员权限），解码数据和日志写在程序目录下。

### 新功能 · New
- **Pluto USB 输入 · Pluto USB input**：*Input* 下拉新增 *Pluto USB*，PlutoSDR 用 USB 线直连即可接收，不需要网口或 IP 地址（通过 libiio 的 USB 后端）。原来的 *Pluto IP* 不变。
  A new *Pluto USB* input: plug the PlutoSDR in with a USB cable, no network address needed (libiio USB backend). *Pluto IP* is unchanged.
- USB 带宽通常比网口低，采样率高时可能丢样本；状态栏会提示溢出，遇到就降低采样率。
  USB usually carries less data than the network link, so a high sample rate can lose samples; the status bar reports overruns, and the rate should then be lowered.
- 同时只接一个 PlutoSDR；Windows 上需要装好 PlutoSDR 的 USB 驱动，并关掉其他占用设备的程序。
  Plug in one PlutoSDR at a time; on Windows the PlutoSDR USB driver must be installed and no other program may be using the device.

GK-2A LRIT 解密密钥不随程序分发 · GK-2A LRIT keys are not included.
