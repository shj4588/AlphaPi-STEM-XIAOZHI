# MyXiaoZhi_MediaServer

> **v1.6.8**（2026-11-12）· 一个容器、一个端口 **8100**，给小智设备供曲：
> 本地音乐 + 在线音乐 + 网络电台 + 本地有声书 + 播放记录 / 有声书续播。
>
> 本项目原名 `R1XiaoZhi_MediaServer`，v1.6.8 起更名为 `MyXiaoZhi_MediaServer`。
> 已部署的旧目录 / 旧容器名可以照常继续用，`files/` 挂卷数据不受改名影响。

---

## 一、整套方案长什么样

| 部件 | 跑在哪 | 端口 | 作用 |
|---|---|---|---|
| **MyXiaoZhi_MediaServer** | NAS / 任意 Docker 机 | **8100** | 曲库总出口：本地音乐、在线音乐、网络电台、有声书、播放记录 |
| **落雪音乐 lxserver** | NAS / 任意 Docker 机 | **9527** | 在线取链通道（搜索和取播放链接都走它）；不装 = 在线歌播不了 |
| 小智服务端 | 官方或自建 | 8003 / 8000 | 语音大脑；官方 `api.tenclass.net`，自建即 xiaozhi-esp32-server |
| 设备端 | R1 音箱 / AlphaPi STEM | — | 播放器；控制台或配网门户指到 8100 |

```
设备 ──语音 WS── 小智服务端
  │
  ├─ R1 音箱        → http://<NAS IP>:8100/index.json       （原始 mp3/m4a，音箱自己解码）
  └─ AlphaPi STEM   → http://<NAS IP>:8100/ap/play          （服务端 ffmpeg 现转 8bit/24k 裸 PCM）

8100 MediaServer ── 落雪 lxserver:9527 ── 各音乐平台
```

服务端会**按客户端身份自动分流**：R1 拿原始 mp3/m4a；AlphaPi 一律转成
**8bit / 24000Hz / 单声道**（协控芯片 1 字节 = 1 样本，只认这个格式）。
识别依据优先级：`?fmt=` 参数 → `/ap/` 路径前缀 → `AP_TRANSCODE` → `?dev=ap` → UA。

---

## 二、快速安装（5 步）

> **在 Podroid / arm64 Linux 虚拟机上部署**（QEMU SLIRP、`r1net`、落雪服务名、apk/npm 换源、
> 旧版升级迁移配置）另见 [`docs/Podroid部署教程_MyXiaoZhi_MediaServer与落雪.md`](docs/Podroid部署教程_MyXiaoZhi_MediaServer与落雪.md)，
> 配套 compose 可直接用 [`docs/docker-compose.podroid.yml`](docs/docker-compose.podroid.yml)。
> ★ Podroid 是虚拟机：**外部访问必须先在 App 设置里加两条端口映射 `8100→8100`、`9527→9527`**
> （默认只转发 SSH 的 `9922→22`），详见教程 §1.1；NAS 部署不需要这一步。

1. 一台能跑 Docker 的机器（群晖 Container Manager、飞牛、PVE 里的 Linux、x86 小主机都行），与设备同一局域网。
2. **装落雪 lxserver**（第三节）—— 只有在线音乐需要，本地曲库 / 有声书 / 电台不受影响。
3. 把 `MyXiaoZhi_MediaServer/` 整个目录拷到 NAS，例如 `/volume1/docker/MyXiaoZhi_MediaServer`，编辑 `docker-compose.yml` 里两个挂载：

   ```yaml
   - /volume1/music:/music                              # 左边改成 NAS 上你的音乐目录
   - /volume2/.../有声读物:/audiobook                    # 左边改成你的有声书目录（没有就删掉这一行）
   ```

4. 起容器（会占用 8100；如果上面还跑着旧的 `r1-music` / `mini-lx` 容器，先 `docker stop r1-music mini-lx && docker rm r1-music mini-lx`）：

   ```bash
   cd /volume1/docker/MyXiaoZhi_MediaServer
   docker compose up -d --build
   ```

5. 浏览器打开 `http://<NAS IP>:8100` → **音源** 页：填落雪地址、开三块开关、选默认音质；再到 **曲库** 页确认本地歌和电台都在。

> `files/` 是挂卷：音源配置、在线池、标签缓存、电台清单、播放记录都在里面，**容器重建不丢**。
> 首次扫 1000+ 首本地歌要等一会儿，扫完自动缓存秒回；往 NAS 传了新歌就在 Web 点 `/reindex`。
> 从旧版升级：因为目录名和 `container_name` 都变了，先在旧目录执行 `docker compose down`（只删容器，不动 `files/`），再到新目录 `up -d --build`。

---

## 三、落雪音乐 lxserver 安装（在线取链通道）

> 在线歌不是本容器自己爬源，而是**把取链整体中继给落雪**。落雪没部署 / 没在 8100 音源页填上地址
> = 播不了在线歌；本地曲库、有声书、网络电台**照常用**。

### 3.1 docker-compose（推荐）

在一个空目录建 `docker-compose.yml`：

```yaml
services:
  lxserver:
    image: xcq0607/lxserver:latest
    container_name: lxserver
    restart: unless-stopped
    ports:
      - "9527:9527"
    volumes:
      - ./data:/server/data
      - ./logs:/server/logs
      - ./cache:/server/cache
    environment:
      - TZ=Asia/Shanghai
```

```bash
docker compose up -d
```

### 3.2 或单条 docker run

```bash
docker run -d -p 9527:9527 \
  -v $(pwd)/lxdata:/server/data -v $(pwd)/lxlogs:/server/logs -v $(pwd)/lxcache:/server/cache \
  --name lx-sync-server --restart unless-stopped xcq0607/lxserver:latest
```

### 3.3 装完必做

1. 打开 `http://<NAS IP>:9527/admin` → 系统配置 → **改掉默认管理员密码 `123456`**。
2. 有自定义音源（订阅源）的，在后台按落雪桌面版同样的方式配进去 —— 服务端跑的就是落雪原版 `musicSdk`，手机上用什么源这里就认什么源。
3. 用**具名用户**的话，在落雪后台建一个持久化 API Token，后面 8100 音源页要填「用户名 + Token」；用 open 公开用户就都留空。

### 3.4 在 8100 音源页接上

浏览器 `http://<NAS IP>:8100` → **音源** 页：

| 项 | 填什么 |
|---|---|
| 落雪 docker（lxserver） | `http://<NAS IP>:9527` → 保存（保存时会自动跑一遍连通自检） |
| 用户名 / Token | 具名用户必填；open 用户留空 |
| 三块开关 | 本地音乐 / 在线音乐 / 网络电台，关掉哪块设备上立刻「没有」，不用重启 |
| 默认音质 | `128k`（默认，最稳最快）/ `192k` / `320k` |

搜索也走落雪：`/api/search` 先问落雪，一个都没搜到才回落脚本音源，这样「搜到的 rid」和「取链」同源，不会出现搜得到却播不了。

### 3.5 验一下

```bash
curl http://<NAS IP>:8100/api/health                       # 曲库服务活着没，看 ver / lx / ap
curl "http://<NAS IP>:9527/api/music/search?name=晴天&limit=5"   # 落雪通不通
curl "http://<NAS IP>:8100/api/lx/selftest"                # 五步自检：首页→出网→搜索→取链→直链验真
```

Web 音源页还有「测一下」按钮，跑的就是 `/api/lx/selftest`，断在哪一环会直接显示。
在线歌播不了 / 每 30 秒切一首，基本都是落雪没配或挂了；总在缓冲就把默认音质改回 `128k`。

---

## 四、compose 挂载与环境变量

### 挂载卷

| 路径 | 内容 | 能不能删 |
|---|---|---|
| `./files:/app/files` | 音源配置（`my_lx`/`media_flags.json`）、在线池 `pool.json`、本地标签缓存、电台清单、`history.json` | **不能删**，删了配置全丢 |
| `/volume1/music:/music` | 本地音乐库 | 按自己 NAS 改 |
| `...:/audiobook` | 本地有声书库（**独立目录，只放有声书**） | 不听本地书可删掉挂载 + `ABL_DIR` 两处 |

### 环境变量（`docker-compose.yml` 的 `environment` 段）

| 变量 | 默认 | 说明 |
|---|---|---|
| `MUSIC_DIR` | `/music` | 容器内音乐目录，与挂载点一致 |
| `ABL_DIR` | `/audiobook` | 容器内有声书目录；换目录要同时改挂载点 |
| `RADIOS_FILE` | `/app/files/radios.json` | 电台清单 |
| `RADIO_PROXY` | `0` | 电台是否走本机代理；默认直连源站（对照实验结论，别随便开） |
| `AP_TRANSCODE` | `auto` | 设备识别：`auto` / `on`（全部当小智转码）/ `off`（退回纯 R1） |
| `AP_RATE` / `AP_BITS` / `AP_CH` | `24000` / `8` / `1` | 小智侧唯一能吃的格式，**不要改** |
| `AP_GAIN_DB` | `0` | 8bit 量化后音量偏小可给 `3` 或 `6`（dB） |
| `AP_START_TIMEOUT_MS` / `AP_IDLE_TIMEOUT_MS` | `20000` / `20000` | 首字节 / 传输期空闲超时 |
| `AP_PACE` | `1` | 匀速发送；设 `0` 回退旧的「ffmpeg 给多少发多少」 |
| `AP_PACE_TICK_MS` / `AP_PREBUF_MS` / `AP_MAXQ_MS` | `20` / `200` / `2000` | 发送节拍 / 预缓冲 / 队列上限（毫秒） |
| `AP_DOWN_TIMEOUT_MS` | `60000` | 「下游不读」多久才收场；**下游没读 ≠ 上游断流**，别调太小 |

> 构建参数：`WITH_FFMPEG=1` 装 ffmpeg（小智侧必需，约 60MB；只给 R1 用可改 `0`）；
> `AP_MIRROR` / `NPM_REGISTRY` 可换 apk / npm 源（arm64 VM 首次构建装 100 个包很慢，实测阿里云 + npmmirror
> 比官方源快一倍以上），**默认留空 = 官方源**，NAS 部署不用改。用法见 Podroid 部署教程 §6.2。
> 容器默认 `user: "0:0"`，因为 SSH 建的 `files/` 常是 root 属主，用 node 用户会写不进配置。

---

## 五、Web 控制台

`http://<NAS IP>:8100`，顶栏显示版本号与三块状态，共三个页：

| 页 | 能做什么 |
|---|---|
| **曲库** | 本地 / 在线 / 电台三块分开看、都能试听；电台增删；`/reindex` 重扫本地曲库和有声书 |
| **音源** | 落雪地址 + 用户名/Token、三块开关、默认音质、「测一下」自检 |
| **播放记录** | 按设备分组看最近 20 条（内容 / 集号 / 进度 / 多久以前），可单台清或全清 |

三块开关的口径：关本地 → `/index.json` 里本地曲目为 0、`/m/*` 全 404；关在线 → `/online.json` 回 `off:true`；关电台 → `radios` 为空数组、语音点电台 = 没有。

**播放记录**：设备靠固件上报的 WiFi MAC（`X-Dev-Mac` 头）区分，老固件没报就退回按来源 IP（换 IP 会被当成新设备）。
只有**有声书**从这里续播，从停下位置**往前 10 秒**起播；歌曲和电台不续。
播放中每 30 秒上报一次心跳位置，意外断电最坏只丢 30 秒。
`GET /api/history` 取记录，`DELETE /api/history?dev=<id>` 清一台，`DELETE /api/history?all=1` 全清。

---

## 六、设备侧怎么接

### 6.1 R1 音箱（小智客户端）

音箱控制台 `http://<音箱IP>:8080` → 设置 → **音乐源**，只留一行：

```
http://<NAS IP>:8100/index.json
```

保存后点「重载」让小智重新拉曲库索引。改过开关 / 点过「换一批」也要重载一次（`http://<音箱IP>:8080/api/act?op=reload`）。
`index.json` 一次吐出本地 + 在线 + 电台，8010 那行删掉。

### 6.2 AlphaPi STEM（小智固件 V2.4.13）

配网门户 `192.168.4.1`（长按 A 键 2 秒）→ 「我的媒体库」填 **NAS 的 IP**（端口默认 8100）→ 说一句 **「启用我的媒体库」**。设备 IP 由 DHCP 分配会变，别写死。
配套固件 V2.4.13 的 9 个设备侧 MCP 工具：语音点歌 / 电台 / 有声书、下一首（集）、停止、查正在播放、语音调进度、读时长与剩余、查 WiFi 信号。
播一集自动接下一集；有声书按 C 暂停会立即上报位置。音频走 `/ap/play` → 8bit/24k 裸 PCM 直进设备 FIFO，设备端零解码零重采样。

---

## 七、接口速查

| 路径 | 用途 |
|---|---|
| `/index.json` | R1 用：本地 + 在线 + 电台一次吐完 |
| `/ap/index.json` | 小智版曲库，`tracks[].url` 已带 `/ap` 前缀 |
| `/ap/play?kind=song\|radio\|book&q=&ep=&idx=&n=&ss=` | 小智统一播放口，返回 8bit/24k 单声道裸 PCM |
| `/ap/pos?q=&ep=&sec=&abs=&fin=&beat=` | 设备上报播放位置 / 播完 / 30s 心跳 |
| `/ap/song?q=` | 语音点歌一步口（搜一首直接推流） |
| `/api/history` | 播放记录（GET / DELETE） |
| `/api/health` | 体检：`ver`、`lx`、`media`、`localCount`、`ap` 转码状态 |
| `/api/whoami` | 这条请求被识别成 `r1` 还是 `alphapi`、依据是什么 |
| `/api/lx/selftest` | 落雪五步自检 |
| `/api/ap/selftest` | ffmpeg 转码自检（正弦波） |
| `/api/search` / `/api/url` | 在线搜歌 / 取链（只走落雪） |
| `/api/lx` / `/api/media` / `/api/quality` | 落雪地址 / 三块开关 / 默认音质读写 |
| `/api/radios` | 电台清单读写（POST 用数组整体覆盖） |
| `/reindex` | 重扫本地曲库 |
| `/m/<path>` / `/r/<source>/<rid>` / `/radio/<b64url>` | 本地文件 / 在线惰性取链 / 电台 |

临时改档位：`?fmt=wav|pcm|mp3`、`?rate=&bits=&ch=&gain=&raw=1`（单次覆盖，不影响全局）。
`/ap/` 前缀下的同名接口一律强制转码。

---

## 八、常见问题

| 现象 | 处理 |
|---|---|
| 在线歌播不了 / 每 30 秒切一首 | 落雪没配或挂了：查 8100 音源页地址 → `curl http://<NAS>:9527/api/music/search?name=x&limit=5` → `/api/lx/selftest` 看断在哪一步 |
| 在线歌总在缓冲 | 音源页把默认音质改回 `128k` |
| 本地中文歌显示成 `ÖÜ½ÜÂ×` 乱码 | 老 MP3 的 ID3 标签声明 ISO-8859-1 实际是 GBK，服务端已自动还原；`files/local_cache.json` 里的旧乱码加载时就地修，不用重扫 |
| 电台播几秒停一下 / 同一句反复播 | 默认 `RADIO_PROXY=0` 直连源站；别开代理，也别让多个设备抢同一路流 |
| 小智侧无声 / 只有杂音 | 确认 `WITH_FFMPEG=1`、`AP_RATE=24000`/`AP_BITS=8`/`AP_CH=1` 三项保持默认，`/api/whoami` 看识别结果，`/api/ap/selftest` 看转码 |
| 播放记录位置不对 / 续播往回跳 | 位置只认设备 `/ap/pos` 上报；「这集播完」只认 `fin=1`，不要拿服务端推出去的字节数判断（ffmpeg 比实时快十几倍）。v1.6.8 已把集尾断点夹值从 `dur-30` 改为 `dur-3`，配套固件 V2.4.13 |
| 容器重建后配置全没了 | `files/` 没挂卷，或被 `docker cp` 到镜像里了；compose 里必须有 `- ./files:/app/files` |
| 两个容器互相隔离、取链每档超时 | 落雪与曲库要能互相访问；用 `/api/lx/selftest` 五步定位，别只凭绿灯判断 |

日志在 `docker logs MyXiaoZhi_MediaServer`；`/ap/log` 会把设备日志追加到 `files/devlog.txt`，Web 播放记录页也能看。

---

## 九、版本与升级

| 版本 | 要点 |
|---|---|
| v1.4.0 | R1 / AlphaPi 双设备共用一套地址，按身份分流 + ffmpeg 转码 |
| v1.5.x | `/ap/song` 语音点歌、`/ap/play` 三类统一出口（歌单 / 电台 / 有声书） |
| v1.6.0 | 按 MAC 分设备的播放记录、有声书续播、Web 播放记录页 |
| v1.6.3 | 播放位置只认设备上报，不再信服务端字节数 |
| v1.6.4 | 30 秒心跳 + 墙钟封顶，意外断电最多丢 30 秒 |
| v1.6.6 | 匀速发送（墙钟补量）+ 撤销 fin 拦截，修在线歌偶发卡顿 |
| v1.6.7 | 下游 60s 未读单独超时，避免起流 20s 被误杀 |
| **v1.6.8** | 集尾起点夹值 `dur-30` → `dur-3`，修「播完不跳下一集、倒回 28 秒循环」；项目更名 `MyXiaoZhi_MediaServer` |

> **从旧版升级 / 更名**：v1.6.8 起目录与 `container_name` 改为 `MyXiaoZhi_MediaServer`。
> 若 NAS 上原来以 `R1XiaoZhi_MediaServer` 部署过，先在旧目录执行 `docker compose down`
> （只停容器，**不删 `files/`**），再到新目录 `docker compose up -d --build`，否则两个容器会抢
> 8100 端口。也可以保留旧目录名、把 `container_name` 改回旧值，功能完全一致。

改完代码部署：`docker compose up -d --build`。回归测试：

```bash
node _tests/ap-history.js && node _tests/ap-play-args.js && node _tests/ap-transcode.js
```

---

## 十、免责

本项目为个人研究性质的自建媒体服务，仅供学习交流，请勿用于商业用途。
音乐、有声书资源因版权原因不随包提供，请自备；在线音源请遵守各平台条款。使用本方案产生的任何后果由使用者自行承担。
