# Development Scripts

This directory contains helper scripts for local development, testing, asset conversion, and firmware packaging.

These scripts are not required for normal firmware usage and are not part of the JiShi commercial firmware.

## Notes

- `release.py` is used for firmware packaging in the open-source build workflow.
- `audio_debug_server.py` is a local audio debugging helper. It is intended for development and testing only.
- `acoustic_check/` contains local acoustic test utilities. It is intended for development and testing only.
- Do not expose local debugging ports or test tools to public networks.
- Do not put real tokens, secrets, passwords, or private server addresses into these scripts.

# 开发辅助脚本

本目录包含本地开发、测试、资源转换和固件打包相关的辅助脚本。

这些脚本不是正常使用固件所必需的功能，也不属于 JiShi 商业版固件能力。

## 说明

- `release.py` 用于开源构建流程中的固件打包。
- `audio_debug_server.py` 是本地音频调试辅助工具，仅用于开发和测试。
- `acoustic_check/` 包含本地声学测试辅助工具，仅用于开发和测试。
- 不要把本地调试端口或测试工具暴露到公网。
- 不要在这些脚本里写入真实 token、secret、password 或私有服务器地址。
