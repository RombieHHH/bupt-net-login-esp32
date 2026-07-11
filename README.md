# BUPT-Net-Login-ESP32

北邮校园网自动登录器 — 基于 ESP32-C3，使用 ESP-IDF v6.0.2 开发。

## 概述

本项目实现北邮校园网（BUPT-portal）的自动认证登录，运行在 ESP32-C3 微控制器上。设备连接到 `BUPT-portal` 开放 WiFi 后，自动完成 web 认证流程，并定期保活。

**主要功能：**

- WiFi 自动连接 `BUPT-portal`
- HTTP 探测 `generate_204` 检测登录状态
- 自动完成 web 认证（Cookie 获取 + POST 登录 + 验证）
- 凭据加密存储（硬件 HMAC 派生密钥 + NVS AES-XTS 加密）
- 首次运行时串口交互输入学号密码
- 可配置保活间隔（登录状态定期探测）q
- 断线自动重连
- IPv6 SLAAC 地址获取与连通性测试（ping 2400:3200::1）

## 硬件要求

- ESP32-C3 开发板（已验证）
- USB 串口线（烧录与日志输出）

## 构建与烧录

```bash
idf.py set-target esp32c3
idf.py menuconfig    # 开启 IPv6
idf.py build
idf.py -p COMX flash monitor
```

### menuconfig 选项

- `Component config → LWIP → Enable IPv6` — 启用
- `Component config → LWIP → Enable IPv6 stateless address autoconfiguration (SLAAC)` — 启用

## 首次运行

烧录后打开串口监视器，程序会提示输入学号和密码：

```
[INFO   ][00:00:00] No credentials in NVS, please enter:
Username: 202xxxxxx       ← 输入学号
Password:                  ← 输入密码
Save to NVS? (y/n): y      ← 保存到 Flash
```

凭据保存后下次启动自动读取，不再需要输入。

### 凭据安全

账号密码存放在加密的默认 NVS 分区。ESP32-C3 使用 eFuse 中的 HMAC 密钥在
运行时派生 AES-XTS 密钥，密钥本身不会写入 Flash，也不依赖简单的设备标识混淆。
启用此功能前应先完整擦除芯片，不兼容旧版固件写入的明文 NVS 数据。

首次启动会在空闲的 eFuse `BLOCK_KEY0` 中生成 HMAC 密钥。写入 eFuse 是不可逆的；
已经将该密钥块用于其他用途的设备，应先在 `menuconfig` 中选择另一个空闲密钥块。

## 认证流程

1. WiFi 连接 `BUPT-portal`（无密码，WIFI_AUTH_OPEN）
2. GET `http://connect.rom.miui.com/generate_204?cmd=redirect&arubalp=12345`
   - 204 → 已登录，跳过
   - 302 + Location 包含 `10.3.8` → 未登录，执行认证
3. GET Location URL → 获取 Set-Cookie
4. URL 中 `index` 替换为 `login`
5. POST login_url（Cookie + `user=xxx&pass=xxx`）
6. GET generate_204 验证 → 期望 204 确认登录成功

## 输出示例

```
[INFO   ][00:00:00] Loaded credentials from NVS
[INFO   ][00:00:00] User: 202xxxxxxx
[INFO   ][00:00:00] Connecting to BUPT-portal...
[INFO   ][00:00:04] Got IP: 10.129.xxx.xxx
[INFO   ][00:00:04] Probing...
[INFO   ][00:00:04] Probe status: 204
[INFO   ][00:00:04] Already logged in
[INFO   ][00:00:04] IPv6 global: 2001:da8:215:xxxx:xxxx:xxxx:xxxx:xxxx
[INFO   ][00:00:06] IPv6 ping: OK (0% loss)
[INFO   ][00:00:06] Sleeping 120s...
```

## 参考与许可

本项目参考了 [YouXam/bupt-net-login](https://github.com/YouXam/bupt-net-login) 的认证逻辑实现。

Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

See the [LICENSE](LICENSE) file for details.
