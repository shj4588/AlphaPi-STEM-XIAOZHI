# 第三方组件与来源声明（Third-Party Notices）

本固件（`AlphaPi_Xiaozhi_V2.4.16.ino`）**自己的代码**以 **AGPL-3.0-only** 发布（全文见 [`LICENSE`](LICENSE)）。
下面列的是它**用到的别人的东西**，以及**与本项目有渊源、但不含其代码**的项目。
分发固件或刷机页时请连同本文件一起带上 —— LGPL / MIT / BSD 都要求保留声明。

---

## 一、编译进固件的依赖库

| 组件 | 头文件 / 来源 | 许可 | 版权 |
|---|---|---|---|
| **ESP32 Arduino core** | `WiFi.h` `WiFiClientSecure.h` `HTTPClient.h` `WebServer.h` `Preferences.h` | **LGPL-2.1** | Espressif Systems / Arduino 社区（[espressif/arduino-esp32](https://github.com/espressif/arduino-esp32)，`LICENSE.md`） |
| **arduinoWebSockets** | `WebSocketsClient.h` | **LGPL-2.1** | Markus Sattler（[Links2004/arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets)） |
| **libopus** | `opus.h` | **BSD-3-Clause**（Opus 另附免专利费授权声明） | Xiph.Org / Skype / Octasic 等（[xiph/opus](https://github.com/xiph/opus)，`COPYING`） |

**LGPL-2.1 的静态链接义务**：这几个库是静态链接进固件的，按 LGPL §6 要让拿到固件的人
能用「修改过的库」重新链接。本项目**全部源码公开（AGPL）**，任何人改完库后用第五章的工具链重新编译、
用同一个刷机页烧回去即可，义务已满足 —— 无需额外提供目标文件。

---

## 二、刷机网页工具（浏览器端，不编进固件）

| 组件 | 许可 | 说明 |
|---|---|---|
| **esptool-js 0.6.1** + Web Serial API | **Apache-2.0** | [espressif/esptool-js](https://github.com/espressif/esptool-js)，`AlphaPi_Xiaozhi_Flasher_V2.4.16.html` 里从 unpkg 在线加载 |

页面里**内嵌的 `.bin` 是本固件的编译产物**，属 AGPL-3.0 —— 对应源码就是同仓库的 `.ino`，页脚已注明。

---

## 三、协议参考（**不含其代码**）

### 3.1 小智原项目 `78/xiaozhi-esp32` —— MIT

本项目是**独立实现**：Arduino 单文件 C++，与上游的 ESP-IDF 多文件工程没有代码继承关系。
只是遵循它公开的 **WebSocket 文本 / 二进制帧格式与 JSON 字段**（`hello` / `listen` / `tts` / `stt` 等）——
这类协议事实本身不受版权保护，任何客户端都可以实现。

- 若你在本固件基础上**拷贝了该项目的代码片段**，那些片段仍归原作者、按 **MIT** 使用，
  必须保留其版权与许可声明；
- **MIT 与 AGPL-3.0 兼容**：MIT 代码可以并入 AGPL 项目（整体按 AGPL 分发、同时保留 MIT 声明），
  **反向不成立** —— 本项目不能、也没有给上游重新授权，AGPL 只约束本仓库自己的代码；
- 上游是**真开源（MIT）**，商用完全合法；想闭源卖盒子的人直接去用上游即可，不需要本项目的商业授权。

> ⚠️ 别把「本项目非商业 / 商业授权」的说法套到上游头上：那是本项目作者自己的选择，与
> `78/xiaozhi-esp32` 及 Espressif 无关。也**不要暗示**本项目获得原作者或厂商的官方认证。

上游 MIT 全文（原文照录）：

```
MIT License

Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
Copyright (c) 2025 Project Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### 3.2 One S 小智固件（第三方作者，非厂商官方）

README 里多处引用它对 **N32 增益档 3 破音** 的判读、`if (level == 3) level = 4;` 的处理方式，
以及协控 `0x91` 应答等链路细节。这些属于**行为与结论的参考**，本仓库**未拷贝其代码**。
该固件**未标注开源许可**（未授权即默认保留所有权利），因此：

- 本仓库**不包含、也不分发**它的任何代码或二进制；
- 想引用它的代码，请先取得原作者授权；
- 若你认为本项目某段实现与你的代码实质相同，请开 issue，核实后立刻改或删。

### 3.3 小智服务端

官方 `xiaozhi.me` / 自建 `xiaozhi-esp32-server` 与本固件是**两个独立程序**，各按自身许可，
本固件只是它的客户端，不包含服务端代码。

---

## 四、硬件与文档

| 内容 | 版权 / 许可 |
|---|---|
| 厂商开发参考手册（`OneS开发者参考手册.pdf` / `AlphaPi_Zero_开发者参考手册.pdf`） | 归厂商所有，仅作参考，**不在本仓库许可内、也不随仓库分发** |
| 协控（N32）协议帧格式、增益表等描述 | 来自厂商公开手册与第三方作者分析，仅用于个人研究与设备自用；厂商如有异议请联系删除对应章节，不影响代码部分的 AGPL 授权 |
| 音乐 / 有声书内容 | 与本项目无关，请自备并遵守各平台条款 |

---

> 本文件随项目更新；发现遗漏或错标，欢迎 issue 指出。
