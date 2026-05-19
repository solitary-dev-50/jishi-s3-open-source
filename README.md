# JiShi S3 开源基础版固件

本项目是**基于小智开源 ESP32 固件改造的 JiShi S3 开源基础版固件**。

本仓库不是小智官方项目，也不代表小智官方设备端固件。JiShi S3 在小智开源固件的基础上，保留了 JiShi S3 所需的基础硬件适配、协议兼容和构建链路。

## 开源基础版说明

本仓库只包含 JiShi S3 的**设备端固件基础版**，不包含完整 JiShi 产品形态。

本仓库**不包含**以下内容：

- 完整声源定位跟随策略
- 唤醒转头策略
- 云台动作节奏和交互动作编排
- 儿童陪伴策略
- 长期记忆系统
- 商业交付材料
- 私有提示词
- 私有服务器配置

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

## 当前保留能力

- Wi-Fi 配网和联网
- 离线唤醒词检测
- Opus 音频输入输出
- WebSocket 语音通信
- OLED 状态显示
- OV5640-AF 基础拍照
- 舵机基础控制
- LED 状态反馈

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

## 开源说明

本项目保留原开源项目许可证文件，详见 `LICENSE`。

第三方组件、ESP-IDF 组件和托管组件分别遵守其自身许可证。
