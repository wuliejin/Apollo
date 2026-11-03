# 本 fork 的手柄与触觉支持

Apollo 和 Moonlight 的 `master` 包含原有 TCP 视频传输修改，以及以下 Windows 手柄扩展：

- 保留 Xbox One、Xbox Series、DualSense 等客户端型号，而不是统一模拟成 Xbox 360。
- Xbox 机身及左右扳机四马达震动反馈。
- Sony 普通震动、灯条、自适应扳机、触摸板、体感和电量处理。
- DualSense 虚拟 USB HID + 音频复合设备，以及 48 kHz / S16LE 的双声道 actuator PCM 回传。

## 获取完整源码

```sh
git clone --recurse-submodules https://github.com/wuliejin/Apollo.git
git clone --recurse-submodules https://github.com/wuliejin/moonlight-qt.git
```

已有克隆可以运行 `git submodule update --init --recursive`。
Apollo 的 common-c 使用本账号仓库中的 `apollo-libvirtualgamepad` 分支，
Moonlight 的 common-c 使用同一仓库的 `master`；两个主仓库都固定了具体提交，
普通递归克隆会取得对应版本。

Windows 编译仍按 [现有构建文档](building.md) 配置依赖；Moonlight 按其 README 编译。
主机选择 `auto` 时按客户端报告的型号创建手柄；也可手选 `xone`、`xseries`、`ds4`、`ds5` 或 `switch`。

## Windows 驱动

源码仓库包含 MIT 许可的 libvirtualgamepad 客户端与协议头，不包含已签名的驱动二进制。
主机需单独安装匹配的 libvirtualgamepad VHF 驱动；DualSense 波形触觉还需 usbip-win2 传输驱动。
客户端需使用支持这些扩展的本 fork Moonlight。

已验证组合为 libvirtualgamepad `v0.1.0-beta.10`（协议 2，驱动 0.1.0.92，
源码 `5a3e4d222dd0b87556d4c564eb58b450ab579fb0`）及 usbip-win2 0.9.8.1。
本地验证使用从 Vibepollo 2.0.4 签名发行包提取的驱动组件；无需将 Apollo 替换为 Vibepollo。
安装驱动前应核对包的来源、签名及版本。

未安装新驱动时，既有 ViGEm 后端仍可用于 Xbox 360 / DS4；显式选择新型号会报告缺失驱动。

## 验证范围

2026-11-03 已在 USB Xbox Series 实物上验证型号识别、A/B、摇杆、双扳机输入，
以及机身和左右扳机独立震动的 Apollo → Moonlight → 手柄完整回传。
SDL 的 Windows Raw Input 后端使用 WGI 输出扳机震动，测试时应保持串流窗口在前台。

DualSense 的 USB 描述符、Sony feature、分片 URB、unlink、音频节奏和 actuator PCM
提取软件测试已通过；Windows 虚拟 USB 音频端点和 WASAPI 波形通路已验证。
尚无 DualSense 实物验收，不能将上述验证视为实际高清触觉、体感或自适应扳机效果的保证。

这是一套手柄协议模拟与反馈回传方案，不是 USB 整机透传。部分应用仍会显示通用 XInput 名称或图标。

启用详细客户端震动日志可设置 `MOONLIGHT_GAMEPAD_TRACE=1`。
配置 Sunshine 测试构建后，可以通过
`ctest --test-dir build/tests -R test_dualsense_usbip_protocol --output-on-failure`
运行独立 USB/IP 协议测试；将 `build` 替换为实际构建目录。

## 来源与许可

- [Nonary/libvirtualgamepad](https://github.com/Nonary/libvirtualgamepad)：MIT，vendored 文件及许可证在 `third-party/libvirtualgamepad/`。
- DualSense USB/IP 和客户端波形后端保留 GPL-3.0-or-later 标记；USB 描述符及传输 ABI 的额外声明见 `src/platform/windows/dualsense_usbip_NOTICES.txt`。
- [SAxense](https://apps.sdore.me/SAxense)：蓝牙 DualSense 触觉研究及 packet 编码参考，客户端 `third-party/saxense/` 保留原始来源与许可。
