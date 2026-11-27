# NADIR Station

**气象卫星接收与处理软件 · A meteorological-satellite reception and processing station**

by AGcov (BI2QYN) · 版本 2.0.0 · [AGPL-3.0](LICENSE)

NADIR Station 是一套运行在 Windows 上的 L 波段气象卫星地面站软件。它从 SDR 采集基带信号，完成解调、同步、纠错和数据层解析，再生成图像与 L1/L2 产品。整条链路自己实现，接收软判决信息，不依赖其他解码器。

NADIR Station is a Windows ground-station application for L-band weather satellites. It takes baseband from an SDR (or a recording), demodulates, synchronises, error-corrects and parses the data layer, and turns the result into imagery and Level-1/Level-2 products. The whole chain is implemented here and carries soft decisions end to end.

---

## 支持的链路 · Supported downlinks

| 卫星 · Satellite | 链路 · Link | 频率 · Frequency | 调制与编码 · Modulation & coding |
|---|---|---|---|
| FY-2G / FY-2H | S-VISSR | 1687.5 MHz | BPSK 660 ksym/s, NRZ-M, PN 同步 |
| GK-2A | LRIT | 1692.14 MHz | BPSK 128 ksym/s, Viterbi r=1/2, RS(255,223) |
| GK-2A | HRIT | 1695.4 MHz | QPSK 3 Msym/s, Viterbi r=1/2, RS(255,223) |
| ELEKTRO-L | LRIT | 1691.0 MHz | BPSK 294 ksym/s, Viterbi, RS |
| ELEKTRO-L | HRIT | 1691.0 MHz | OQPSK 1.15 Msym/s, Viterbi, RS |
| ELEKTRO-L | GGAK 空间环境遥测 | 1693.0 MHz | PM/BPSK 5 kBd |
| GOES-N/O/P | GVAR | 1685.7 MHz | BPSK 2.111 Msym/s, NRZ-S |
| GOES-N/O/P | Sensor Data (SD) | 1676.0 MHz | BPSK 2.6208 Mbit/s, NRZ-M |
| MetOp | AHRPT | 1701.3 MHz | QPSK 2.33 Msym/s, Viterbi r=3/4, RS |
| METEOR-M | HRPT | 1700.0 MHz | PM / Manchester 665.4 kbit/s |

接收设备：ADALM-PLUTO（libiio，默认地址 `192.168.3.10`，可在 CMake 中用 NADIRSTATION_DEFAULT_PLUTO_IP 修改）、RTL-SDR（运行时加载 `rtlsdr.dll`），也可以回放 `.cs16 / .cs8 / .cu8 / .wav` 录音。

Receivers: ADALM-PLUTO (libiio, default `192.168.3.10`, change with NADIRSTATION_DEFAULT_PLUTO_IP), RTL-SDR (loads `rtlsdr.dll` at run time), or playback of `.cs16 / .cs8 / .cu8 / .wav` recordings.

## 解码门限 · Decoding thresholds

同一份合成基带（真实链路格式，Es/N0 精确）分别交给 NadirStation 和一个逐级硬判决的传统解码方案，门限取正确帧比例跌破 99% 时的 Es/N0。方法和逐档数据见 [docs/THRESHOLDS.md](docs/THRESHOLDS.md)。

The same synthetic baseband (real link formats, exact Es/N0) is fed to NadirStation and to a conventional stage-by-stage hard-decision decoder; the threshold is the Es/N0 where the share of correct frames drops below 99 %. Method and every measured point: [docs/THRESHOLDS.md](docs/THRESHOLDS.md).

| 链路 · Link | NadirStation | 传统硬判决方案 · Conventional | 提升 · Gain |
|---|---|---|---|
| GK-2A LRIT | -1.5 dB | 1.6 dB | +3.2 dB |
| ELEKTRO-L LRIT | -1.6 dB | 1.4 dB | +2.9 dB |
| GK-2A HRIT | 1.5 dB | 3.7 dB | +2.2 dB |
| ELEKTRO-L HRIT | 1.5 dB | 2.9 dB | +1.4 dB |
| MetOp AHRPT | 4.5 dB | 5.0 dB | +0.5 dB |
| FY-2G S-VISSR | -7.6 dB | 4.7 dB | +12.3 dB |

## 功能 · Features

**接收 · Receive**
- **Live**：频谱与瀑布图、星座图、符号 SNR 趋势、锁定状态；IQ 录制（cs16/cs8/cu8/WAV，可限时）。
- **Decoder**：把整条接收链画成 IQ → 载波 → 解调 → 帧同步 → 纠错 → 数据层 → 产品 七个环节，实时显示每一环的健康状态（绿/黄/红）和原因。页面还有 5 分钟健康时间线、数据损失漏斗、各环节的示波器与指标，以及状态变化日志。第一个变红的环节就是故障所在。
- **Live** shows the spectrum and waterfall, the constellation, the symbol-SNR trend and lock state, and records IQ.
- **Decoder** shows every stage of the receive chain live, with its health and the reason. It also has a 5-minute health timeline, a loss funnel, a per-stage scope with metrics, and a change log.

**产品 · Products**
- FY-2 S-VISSR：VIS/IR1–IR4 定标亮温、伪彩、经纬网与海岸线、区域产品（云检测、云顶温度、云分类、分裂窗、水汽增强、对流/过冲云顶、夜间雾等）、L1B NetCDF 导出。
- GK-2A LRIT/HRIT：全圆盘图像（HRIT 含 JPEG2000/JPEG 解码）、定标、ADD 附加数据（日程、文本、天气图）。
- ELEKTRO-L LRIT/HRIT：xRIT 图像（小波/JPEG/T.4 解压）与 L2 合成。
- GOES GVAR：可见光与 4 个红外通道、伪彩、Block 0 遥测；GOES SD：成像仪扫描拼接、亮温、L2 工具（`tools/sd_l2.py`）。
- MetOp AHRPT：AVHRR、MHS、AMSU、IASI、ASCAT 等仪器数据，地图投影与 RGB 合成。
- METEOR HRPT：MSU-MR 图像与投影。
- **Globe**：GK-2A 图像贴在三维地球上，附带离线运行的 **AI 云顶临近预报**（运动估计 + 半拉格朗日平流 + 小型神经网络校正）。

**遥测与规划 · Telemetry & planning**
- FY-2 DOC / MANAM / 定标遥测，GOES GVAR Block 0。
- ELEKTRO-L GGAK 日地物理遥测与空间天气视图。
- **Passes**：SGP4 过境预报、天空图、星下点轨迹、实时方位/仰角/多普勒。
- **System**：CPU、内存、磁盘、接收机状态。

## 构建 · Building (Windows)

需要 · Requirements:
- Qt 6（MinGW 64-bit 套件，已在 Qt 6.11 + MinGW 13.1 上测试）与 CMake ≥ 3.21（随 Qt 安装）
- 可选：[libiio](https://github.com/analogdevicesinc/libiio)（PlutoSDR 支持）

```bat
rem 按需修改 build_windows.bat 顶部的 QT_DIR / MINGW_DIR / LIBIIO_ROOT，然后：
build_windows.bat
```

生成的 `build\NADIR_Station.exe` 已带上 Qt 运行库和 `resources\`。没有 libiio 时会自动编译为仅支持 RTL-SDR 与文件回放的版本。

The output `build\NADIR_Station.exe` is deployed with the Qt runtime and `resources\`. Without libiio the build falls back to RTL-SDR and file playback only.

Linux（实验性 · experimental）：`./build_linux.sh`

## 未随源码分发的文件 · Not included

- **GK-2A LRIT 解密密钥**：KMA 对部分 LRIT 文件加密，密钥文件 `EncryptionKeyMessage.bin` 需要自行获取，然后放到 `resources/gk2a/`（与程序同级）。
- **录音样本**：体积过大，不在仓库中。
- GK-2A LRIT encryption keys (`EncryptionKeyMessage.bin`) are not distributed. Place your own copy in `resources/gk2a/` next to the executable. Sample recordings are not in the repository.

## 目录 · Layout

```
src/            应用程序与解码器源码 · application and decoders
tools/          命令行探针、阈值测试、Python 产品工具 · CLI probes, threshold tests, Python tools
resources/      地图、查找表、轨道、模型、图标 · maps, LUTs, orbits, model, icons
third_party/    sgp4, OpenJPEG, libjpeg-turbo, PublicDecompWT（保留各自许可证）
```

## 许可 · License

Copyright (C) 2026 AGcov (BI2QYN)

本程序是自由软件，按 **GNU Affero General Public License v3.0** 发布，见 [LICENSE](LICENSE)。如果你修改本程序并通过网络向他人提供服务，必须向这些用户提供修改后的完整源码。第三方组件与资源保留各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

This program is free software, released under the **GNU Affero General Public License v3.0** ([LICENSE](LICENSE)). Third-party components and assets keep their own licenses, listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## 免责声明 · Disclaimer

本软件为业余接收与研究用途。卫星数据归各发布机构（CMA/NSMC、KMA、Roshydromet、NOAA、EUMETSAT 等）所有。本站自行解码的图像和产品未经官方质量控制，不能替代官方产品；AI 临近预报是实验性结果，不能用于任何安全相关的决定。

For amateur reception and research. Satellite data belong to their operators. Products decoded here have no official quality control and do not replace official products. The AI nowcast is experimental and must not be used for safety-related decisions.

问题与建议请提交 [GitHub Issues](https://github.com/AGcovStudio/NadirStation/issues)。
