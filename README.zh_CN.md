**简体中文** · [English](README.md)

# FoloTOTP —— TOTP 验证器固件

FoloTOTP 把 FoloToy AI Passport 可穿戴设备(ESP32-C3、8MB Flash、240×320
屏幕、三按键)变成一台口袋 TOTP(RFC 6238)验证器,带手势密码、设备端
Wi-Fi 配网与局域网远程密钥管理。它是
[FoloToy AI Passport](docs/README.zh_CN.md) 基线的派生应用:硬件测试演示
仅作参考保留,本仓库固件编译的是应用本体。

## 功能特性

- **手势密码** —— 首次开机设置 8 步"上/下"序列(2^8 = 256 种组合);只存
  加盐哈希,不存序列本身。
- **锁定策略** —— 15 分钟无按键自动锁定;连续输错 10 次锁定 60 分钟,锁定
  页带倒计时。失败计数与惩罚截止时间持久化,断电重启不能绕过。
- **验证码** —— RFC 6238 动态码每 30 秒刷新并带倒计时条;上/下键翻页查看。
- **设备端配网** —— 设备直接扫描周围网络,选中后用两级三键键盘(abc / ABC
  / 123 / 符号 分组)输入密码;DHCP 获取 IP。无需手机 App、蓝牙或热点。
- **远程管理** —— 联网后屏幕显示管理网址、对应二维码与随机 8 位数字码;
  同局域网设备打开网址、输入数字码即可增删 TOTP 条目。密钥对网络只写不读。
- **中英双语** —— 设备界面与远程管理网页均支持简体中文/English,菜单内一键
  切换,选择持久化。
- **其他** —— 右上角电量显示、30 秒无操作背光调暗、SNTP 校时(时钟不正确
  时拒绝显示验证码)。

## 硬件要求

| 项目 | 要求 |
| --- | --- |
| 主控 | ESP32-C3(RISC-V),8MB Flash,无 PSRAM |
| 屏幕 | ST7789P3 240×320 SPI,圆角 |
| 按键 | 单 ADC 引脚三键(上/下/确定) |
| 电量计 | CW2017(I²C,可选;不可用时 UI 自动降级) |

引脚定义与硬件事实见
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h)。

## 构建与烧写

需要 [ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/)。激活
环境后:

```bash
./tools/validate.sh --static    # 仓库检查 + 宿主测试
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像验证
./tools/validate.sh             # 完整门禁
```

在偏移 `0x0` 烧写验证过的合并镜像:

```bash
idf.py -p PORT flash            # 日常增量开发
# 或将 build/FoloToy-AI-Passport-full.bin 从 0x0 完整刷入
```

宿主测试覆盖全部纯逻辑模块(TOTP/Base32 对照 RFC 官方向量、手势密码状态
机、两级键盘模型、双语字符串表),任意 C 编译器即可运行,见
`tools/validate.sh`。

## 快速上手

1. 首次开机要求创建手势密码:输入 8 步上/下,再重复确认。
2. 菜单 → 无线网络 → 扫描连接:选网络、两级键盘输密码、长按确定连接;
   设备经 DHCP 联网并自动校时。
3. 菜单 → 远程管理:屏幕显示网址、二维码与 8 位数字码;局域网内任意设备
   打开即可管理条目。
4. 验证码每 30 秒刷新;任意界面长按确定可立即锁定。

完整手册(各页面按键、配网细节、管理会话规则、名称字符策略与安全说明)
见 [docs/totp-app.zh_CN.md](docs/totp-app.zh_CN.md)
([English](docs/totp-app.md))。

## 安全说明

- 256 种组合的手势密码防"随手拿起看一眼",不防拥有无限时间的攻击者。
- 管理服务为局域网明文 HTTP;密钥不会离开设备,但请在可信网络使用。
- 管理会话在设备锁定、5 分钟无活动、输错 5 次数字码或 Wi-Fi 断开时自动结束。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `main/` | 应用:TOTP 核心、锁定状态机、存储、Wi-Fi、HTTP 服务、UI、双语字符串表 |
| `components/bsp/` | 板级支持(显示、按键、电量、音频)—— 可复用硬件层 |
| `assets/fonts/` | 内置思源黑体子集(许可与再生成方式见目录文档) |
| `tests/` | 纯逻辑模块的宿主测试 |
| `docs/` | 产品与开发文档,[应用手册](docs/totp-app.zh_CN.md) |

上游基线、硬件指南与贡献规则见 [AGENTS.md](AGENTS.zh_CN.md) 与
[docs/README.zh_CN.md](docs/README.zh_CN.md)。

## 许可

MIT —— 见 [LICENSE](LICENSE)。内置字体子集由思源黑体(SIL OFL 1.1)生成。
