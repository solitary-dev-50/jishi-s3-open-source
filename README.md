# JiShi S3 Open Source Firmware

Language: English | [中文](#jishi-s3-开源基础版固件)

This project is the **open-source basic version of the JiShi S3 device-side firmware**, based on the open-source Xiaozhi ESP32 firmware.

This is not an official Xiaozhi firmware project, and it does not represent the official Xiaozhi device-side firmware. JiShi S3 keeps the basic hardware adaptation, protocol compatibility, and build flow required for the JiShi S3 board on top of the Xiaozhi open-source firmware.

## Open Source Basic Version

This repository only contains the **device-side basic firmware** for JiShi S3. It does not include the complete JiShi product firmware or the complete JiShi commercial firmware.

## Upstream

This project is based on the open-source Xiaozhi ESP32 firmware:

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)

JiShi S3 is an adapted device-side firmware project and is not the original Xiaozhi project.

## Project Scope

- Only keeps the `jishi-s3` board entry
- Target chip: `esp32s3`
- Only contains device-side firmware
- Compatible with Xiaozhi-compatible backend protocols, but does not include backend services

## Basic Hardware Adaptation

- MCU: ESP32-S3
- Microphone chain: TLV320ADC5140 to I2S
- Speaker chain: MAX98357A I2S
- Display: SSD1306 OLED
- Camera: OV5640-AF
- Servo: basic dual-axis gimbal control
- Status LED: WS2812 LED strip

Main hardware configuration files:

- `main/boards/jishi-s3/config.h`
- `main/boards/jishi-s3/config.json`

## Included Basic Capabilities

- Wi-Fi provisioning and networking
- Offline wake word detection
- Opus audio input and output
- WebSocket voice communication
- OLED status display
- Basic OV5640-AF photo capture
- Basic servo control
- LED status feedback

## Commercial Firmware Capabilities

The full JiShi commercial firmware is designed for a more complete interactive companion device experience, including:

- Looking toward the speaker after wake-up
- Light speaker-following behavior during conversation
- More natural gimbal motion rhythm and idle behavior
- Child-friendly companion interaction design
- Long-term memory and growth record support
- Camera-based visual interaction scenarios
- More complete device behavior orchestration and diagnostics

These capabilities are not included in this open-source basic firmware. This repository only provides the basic device-side firmware for hardware adaptation, protocol compatibility, and developer testing.

JiShi's commercial firmware focuses on turning the device from a basic voice terminal into a productized companion runtime with behavior orchestration, offline fallback, visual interaction, and child-friendly interaction design.

## What Is Not Included

This repository **does not include**:

- complete sound source localization and tracking strategy
- wake-up head-turning strategy
- gimbal motion rhythm and behavior strategy
- child companion strategy
- long-term memory system
- commercial delivery materials
- private prompts
- private server configuration

## Camera Notes

The camera currently used and tested is `OV5640-AF`.

In theory, `OV2640` may be used as an alternative, but this repository has not fully tested `OV2640` as a drop-in replacement.

## Backend Compatibility

This firmware can connect to a backend that implements a Xiaozhi-compatible WebSocket protocol.

Reference open-source backend:

- `xinnan-tech/xiaozhi-esp32-server`

This repository only contains device-side firmware and does not include backend services.

## Directory Structure

- `main/`: main firmware source code
- `main/boards/jishi-s3/`: JiShi S3 board adaptation
- `main/boards/common/`: shared basic board components
- `partitions/`: partition tables
- `scripts/`: build helper scripts
- `sdkconfig`: current project configuration
- `sdkconfig.defaults`: common default configuration
- `sdkconfig.defaults.esp32s3`: ESP32-S3 default configuration

## Build Environment

Recommended environment:

- ESP-IDF 5.5.x
- Python 3.11
- ESP32-S3 target
- Windows PowerShell or ESP-IDF terminal

Build command:

```powershell
idf.py -B build-jishi-s3-open build
```

## Flashing

Replace `COM21` with the actual serial port:

```powershell
idf.py -B build-jishi-s3-open -p COM21 flash monitor
```

## Support

Support the open-source basic firmware: https://ko-fi.com/solitarydev50

## Open Source Notice

This project keeps the original open-source project license file. See `LICENSE`.

Third-party components, ESP-IDF components, managed components, and the original Xiaozhi open-source firmware follow their respective licenses.

---

# JiShi S3 开源基础版固件

语言：[English](#jishi-s3-open-source-firmware) | 中文

本项目是**基于小智开源 ESP32 固件改造的 JiShi S3 开源基础版固件**。

本仓库不是小智官方项目，也不代表小智官方设备端固件。JiShi S3 在小智开源固件的基础上，保留了 JiShi S3 所需的基础硬件适配、协议兼容和构建链路。

## 开源基础版说明

本仓库只包含 JiShi S3 的**设备端固件基础版**，不包含完整 JiShi 产品固件，也不包含完整 JiShi 商业版固件。

## 上游来源

本项目基于小智开源 ESP32 固件改造：

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)

JiShi S3 是基于该上游项目改造的设备端固件项目，不是小智原始项目。

## 项目定位

- 只保留 `jishi-s3` 板型入口
- 目标芯片为 `esp32s3`
- 只包含设备端固件
- 兼容小智协议后端，不包含后端服务

## 基础硬件适配

- 主控：ESP32-S3
- 麦克风链路：TLV320ADC5140 转 I2S
- 扬声器链路：MAX98357A I2S
- 显示：SSD1306 OLED
- 摄像头：OV5640-AF
- 舵机：双轴云台基础控制
- 状态灯：WS2812 灯链

主要硬件配置文件：

- `main/boards/jishi-s3/config.h`
- `main/boards/jishi-s3/config.json`

## 当前保留基础能力

- Wi-Fi 配网和联网
- 离线唤醒词检测
- Opus 音频输入输出
- WebSocket 语音通信
- OLED 状态显示
- OV5640-AF 基础拍照
- 舵机基础控制
- LED 状态反馈

## 完整商业版能力

完整 JiShi 商业版固件面向更完整的桌面陪伴设备体验，包含：

- 唤醒后看向说话人
- 对话期间轻微跟随说话方向
- 更自然的云台动作节奏和待机行为
- 面向儿童的陪伴式交互设计
- 长期记忆和成长记录支持
- 基于摄像头的视觉交互场景
- 更完整的设备行为编排和诊断能力

以上能力不包含在本开源基础版中。本仓库只提供基础设备端固件，用于硬件适配、协议兼容和开发者测试。

JiShi 商业版固件的重点，是把设备从普通语音终端，升级为具备行为编排、离线降级、视觉交互和儿童友好交互设计的产品化陪伴终端。

## 不包含内容

本仓库**不包含**以下内容：

- 完整声源定位跟随策略
- 唤醒转头策略
- 云台动作节奏和交互动作策略
- 儿童陪伴策略
- 长期记忆系统
- 商业交付材料
- 私有提示词
- 私有服务器配置

## 摄像头说明

当前实际使用并调试的是 `OV5640-AF` 摄像头。

理论上 `OV2640` 可能可作为替代方案，但本仓库没有对 `OV2640` 做完整实测验证。

## 后端兼容

本固件可连接实现小智兼容 WebSocket 协议的后端。

可参考的开源后端：

- `xinnan-tech/xiaozhi-esp32-server`

本仓库只包含设备端固件，不包含后端服务。

## 目录结构

- `main/`：固件主代码
- `main/boards/jishi-s3/`：JiShi S3 板级适配
- `main/boards/common/`：公共板级基础组件
- `partitions/`：分区表
- `scripts/`：构建辅助脚本
- `sdkconfig`：当前工程配置
- `sdkconfig.defaults`：通用默认配置
- `sdkconfig.defaults.esp32s3`：ESP32-S3 默认配置

## 构建环境

建议环境：

- ESP-IDF 5.5.x
- Python 3.11
- ESP32-S3 target
- Windows PowerShell 或 ESP-IDF 终端

构建命令：

```powershell
idf.py -B build-jishi-s3-open build
```

## 烧录

将 `COM21` 替换为实际串口：

```powershell
idf.py -B build-jishi-s3-open -p COM21 flash monitor
```

## 支持作者

支持开源基础版固件维护：https://afdian.com/a/solitary-dev-50

## 开源说明

本项目保留原开源项目许可证文件，详见 `LICENSE`。

第三方组件、ESP-IDF 组件、托管组件以及小智原始开源固件分别遵守其自身许可证。

