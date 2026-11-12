# Podroid（Android arm64 虚拟机）部署 MyXiaoZhi_MediaServer + 落雪音乐

> 面向「在一台 Podroid / QEMU arm64 Linux VM 上，从零把曲库服务和落雪服务端跑起来」的场景。
> 服务端版本 **MyXiaoZhi_MediaServer v1.6.8**（端口 8100），落雪 `xcq0607/lxserver`（端口 9527）。
>
> ✅ **2026-11-12 在 Podroid VM 上删掉旧容器完整重建验证通过**：`ver=1.6.8`、落雪五步自检全绿、
> ffmpeg 转码自检 `36000/36000 B`、51 台电台与 100 首在线池完整迁移、LAN 侧 8100/9527 均 200。
> 文中命令与 compose 均为该次实跑所用，可直接照抄。
>
> 本文只写**部署与验证**，不涉及设备端 APK / 固件装机。标准 NAS 部署看项目根目录 `README.md`。
> 文中的 IP、端口、密码、hostkey 是**示例值，按你的 VM 实际替换**。

---

## 0. 先看拓扑，记住三个最容易踩的坑

```
Windows PC ──plink/pscp──→ Podroid 宿主 <VM_IP>:9922 ──SLIRP──→ VM:22
          ──curl --noproxy '*'──→ <VM_IP>:8100 / :9527

┌─ Podroid / arm64 VM（Alpine aarch64）──────────────────────┐
│  Docker + compose                                          │
│                                                            │
│  /opt/my-media    → 容器 MyXiaoZhi_MediaServer :8100        │
│  /opt/lxserver    → 容器 lxserver              :9527        │
│         │                    ▲                             │
│         └── my_lx = http://lxserver:9527 ────┘              │
│              两个容器挂同一个外部网络 r1net                  │
└────────────────────────────────────────────────────────────┘

设备 ──→ http://<VM_IP>:8100/index.json     （R1 音箱）
     ──→ http://<VM_IP>:8100/ap/play       （AlphaPi STEM，配网门户里填 IP）
```

1. **`my_lx` 必须写容器服务名 `http://lxserver:9527`**，不能写 `http://<VM_IP>:9527`。
   VM 是 QEMU SLIRP guest，容器内没有到宿主 LAN 的回程路由；只有两个容器同在 `r1net` 才能按服务名互访。
2. **PC 上 curl 一律加 `--noproxy '*'`**。本机若设了 `HTTP_PROXY`，不加会拿到假 502，误判成服务没起来。
3. **★ 外部访问必须先在 Podroid 设置里加端口映射，8100 和 9527 两条都要**（见 §1.1）。
   SLIRP 默认只把宿主 9922 转到 guest 22；容器在 VM 里监听得再好，宿主没映射外面也连不上。
   guest 实际 IP 一般是 `10.0.2.15`，设备 / PC 用的是**宿主 LAN IP**，不是这个地址。
   不要在 VM 里改 iptables / 改 compose 端口映射去凑 —— compose 的 `8100:8100` 只是 VM 内部的 Docker 层。

---

## 1. 前置条件

| 项 | 示例值 / 要求 |
|---|---|
| Podroid VM | Alpine 3.24、aarch64、建议 ≥ 2C2G（转码要 ffmpeg，4C4G 更稳） |
| SSH | `root@<VM_IP>`，端口 **9922**（宿主转发到 guest 22） |
| 宿主端口映射 | **§1.1 必做**：TCP `8100→8100`、`9527→9527`；默认只有 `9922→22` |
| Docker | 实测 Docker 29.5.3 + Compose v5.1.4；初始镜像为空也能跑 |
| 架构 | arm64：`node:20-alpine` 与 `xcq0607/lxserver:latest` 都有 aarch64 manifest |
| PC 工具 | PuTTY 的 `plink` / `pscp`；curl、tar（Git Bash 即可） |
| 发布包 | `MyXiaoZhi_MediaServer/` 整个目录 + `docs/docker-compose.podroid.yml` |

> 版本要 **v1.6.8**（有按设备分流、ffmpeg 转码、按 MAC 的播放记录与有声书续播）。
> 旧教程的 `R1XiaoZhi_MediaServer_v1.2.4` 已更名并升级，别拿旧包照抄。

### 1.1 ★ 部署前必做：在 Podroid 设置里添加两条端口映射

**这是两层独立的映射，别混在一起：**

| 层 | 谁负责 | 内容 |
|---|---|---|
| Docker 层（VM 内） | compose | `8100:8100`（曲库）、`9527:9527`（落雪），两份 compose 里已写好 |
| QEMU/SLIRP 层（**Podroid 宿主**） | **Podroid App 设置，需手动加** | 默认只有 `9922 → 22`（SSH）；外部要访问服务，得自己加 `8100 → 8100` 和 `9527 → 9527` |

只有第二层通了，`http://<VM_IP>:8100/...` 和 `http://<VM_IP>:9527/...` 才在局域网里可达。
`<VM_IP>` 是 **Podroid 宿主（Android 设备）的 LAN IP**，不是 guest 里的 `10.0.2.15`。

**操作**：打开 Podroid → 选中这台虚拟机 → 设置里的「网络 / 端口映射」（不同 App 版本入口叫法略有差异，
也可能叫「端口转发 / Port Forward / 端口规则」），新增两条 **TCP** 规则：

| 名称（随意） | 协议 | 宿主端口 | 虚拟机端口 | 用途 |
|---|---|---|---|---|
| 曲库 | TCP | **8100** | **8100** | Web 音源页 / `/index.json` / `/ap/play`，设备全部走它 |
| 落雪 | TCP | **9527** | **9527** | `/admin` 后台改密码、导入自定义音源、外部自检验真 |

- 已有的 `9922 → 22` **别删**，SSH / plink / pscp 全靠它。
- 宿主端口与虚拟机端口填**同一个数字**即可（容器内服务监听的就是 8100 / 9527）。
- 保存后若 App 提示要重启 VM 才生效，就重启一次；容器是 `restart: unless-stopped`，会自己回来。

**两个都要加，只加 8100 不够**：两个容器之间走 `r1net` 服务名 `http://lxserver:9527`，
不经过宿主映射，所以只映射 8100 时在线歌可能照样能播，但落雪后台 `/admin` 打不开
（改默认密码、导入自定义音源、建 API Token 都在那里），本教程 §10 里所有 `http://<VM_IP>:9527` 的验收命令也会全红，
很容易误判成「落雪挂了」。

**验证**（从同一局域网的 PC，务必带 `--noproxy '*'`）：

```bash
VM_IP=192.168.2.103     # 换成 Podroid 宿主的 LAN IP
curl -s --noproxy '*' -o /dev/null -w '8100 %{http_code}\n' http://$VM_IP:8100/api/health
curl -s --noproxy '*' -o /dev/null -w '9527 %{http_code}\n' http://$VM_IP:9527/
```

两个都要 `200`。判读：

- VM 内 `wget -qO- http://127.0.0.1:8100/api/health` 正常、PC 侧 `000` / 超时 → 映射没加，或加了没重启 VM；
- PC 侧 `502` → 本机 `HTTP_PROXY` 作怪，加 `--noproxy '*'`；
- 只挂一个端口 → 检查是不是漏了另一条规则。

> 2026-11-12 实测：这台 VM `ss -tln` 显示 `0.0.0.0:8100`、`0.0.0.0:9527`、`0.0.0.0:22` 均监听，
> Podroid 宿主映射生效后 PC 侧 8100 / 9527 均 200；`my_lx` 仍写服务名 `http://lxserver:9527`。

---

## 2. 连接 VM、上传发布包、迁移旧配置

先定义变量（按实际改）：

```bash
PL="/c/Program Files/PuTTY/plink"
PSCP="/c/Program Files/PuTTY/pscp"
VM_IP="192.168.2.103"
PW="podroid"                 # 你的 SSH 密码
HK="SHA256:W3iicbZW+Mc6Gsc0wr4iDoIBl0ASgq10v3kyWApxfE4"   # 首次连接记下的指纹
HOST="root@$VM_IP"
M="F:/玩机/AlphaPi/AlphaPi_STEM/MyXiaoZhi_MediaServer"
```

### 2.1 免交互登录 + 看环境

```bash
"$PL" -ssh -P 9922 -pw "$PW" -hostkey "$HK" -batch "$HOST" \
  "uname -a; cat /etc/alpine-release; docker --version; docker compose version"
```

- `-batch` 禁掉交互提示，`-hostkey` 固定指纹；VM 刚开机 sshd 未就绪时 plink 会挂住，用 `timeout 30` 包一层。
- ★ **长时间无输出的命令会被这条 SSH 链路静默掐断**（实测 `docker compose build 2>&1 | tail -45`
  返回过一次空输出，镜像根本没构建）。构建时用 `docker compose build --progress=plain` **不做管道缓冲**，
  让日志实时流出；或者在 VM 上把输出重定向到日志再单独查。

### 2.2 打包上传（比 `pscp -r` 稳，隐藏文件 `.dockerignore` 不会丢）

```bash
# 排除运行态 files/、旧备份 _stale/、docs/；docs 里的 podroid compose 单独传
tar czf /tmp/myxiaozhi_deploy.tar.gz -C "$M" \
  --exclude=_stale --exclude=files --exclude=docs .

MSYS_NO_PATHCONV=1 "$PSCP" -P 9922 -pw "$PW" -hostkey "$HK" \
  /tmp/myxiaozhi_deploy.tar.gz "$HOST:/opt/deploy_v168.tar.gz"

MSYS_NO_PATHCONV=1 "$PSCP" -P 9922 -pw "$PW" -hostkey "$HK" \
  "$M/docs/docker-compose.podroid.yml" "$HOST:/opt/my-media/docker-compose.podroid.yml"
```

> `.dockerignore` 必须带过去：它把 `files/*` 排除在构建上下文之外，否则运行态配置会被打进镜像，
> 重建后覆盖挂卷里的真实数据。

### 2.3 确认 PC 侧网络没被代理骗

```bash
curl -s --noproxy '*' -o /dev/null -w '%{http_code}\n' http://$VM_IP:8100/api/health
```

部署前返回 `000` 正常；只要**不是 502** 就说明没被本机代理劫持。
若容器已起、VM 内 `wget -qO- http://127.0.0.1:8100/api/health` 也正常，这里却一直 `000`，
先回 §1.1 检查 Podroid 里那两条宿主端口映射加没加。

### 2.4 ★ 升级旧部署：先备份、再迁移 `files/`（全新安装可跳过）

`files/` 里有音源地址、在线池、标签缓存、电台清单、播放记录，**删容器不会删它，但换部署目录会**：

```bash
# ① 备份（VM 上执行）
tar czf /opt/backup_pre_v168.tar.gz -C /opt \
  r1-media/files r1-media/docker-compose.yml \
  lxserver/docker-compose.yml lxserver/data

# ② 停旧容器（旧目录里执行；外部网络 r1net 不会被删）
cd /opt/r1-media && docker compose down

# ③ 解压新包，把旧 files/ 整个复制过来
mkdir -p /opt/my-media && tar xzf /opt/deploy_v168.tar.gz -C /opt/my-media
cp /opt/my-media/docker-compose.podroid.yml /opt/my-media/docker-compose.yml
mkdir -p /opt/my-media/files /opt/my-media/music /opt/my-media/audiobook
cp -a /opt/r1-media/files/. /opt/my-media/files/

# ④ 确认落雪地址仍是服务名
cat /opt/my-media/files/my_lx     # → http://lxserver:9527
```

实测迁移结果：51 台电台、100 首在线池、`media_flags` 三块开关全部原样保留。

---

## 3. 宿主初始化：镜像加速 + 容器 DNS

Docker Hub 在这类 VM 上直连常超时，先配加速源。宿主 `resolv.conf` 常是 `8.8.8.8 / 1.1.1.1`，
容器内 DNS 要 5 秒，会让 `/api/lx/selftest` 偶发全红 —— **DNS 这一项不是可选项**。

`/etc/docker/daemon.json`：

```json
{
  "registry-mirrors": [
    "https://docker.m.daocloud.io",
    "https://docker.1ms.run",
    "https://docker.xuanyuan.me"
  ],
  "dns": ["223.5.5.5", "119.29.29.29"],
  "log-driver": "json-file",
  "log-opts": { "max-size": "10m", "max-file": "3" }
}
```

```bash
"$PL" -ssh -P 9922 -pw "$PW" -hostkey "$HK" -batch "$HOST" \
  "mkdir -p /etc/docker && cat > /etc/docker/daemon.json <<'EOF'
{
  \"registry-mirrors\": [\"https://docker.m.daocloud.io\", \"https://docker.1ms.run\", \"https://docker.xuanyuan.me\"],
  \"dns\": [\"223.5.5.5\", \"119.29.29.29\"],
  \"log-driver\": \"json-file\",
  \"log-opts\": { \"max-size\": \"10m\", \"max-file\": \"3\" }
}
EOF
rc-service docker restart 2>/dev/null || service docker restart"

"$PL" -ssh -P 9922 -pw "$PW" -hostkey "$HK" -batch "$HOST" \
  "docker run --rm alpine:latest nslookup baidu.com"
```

Alpine 用 OpenRC（`rc-service docker restart`）；systemd 发行版换成 `systemctl restart docker`。

> ★ **注意区分**：`registry-mirrors` 只管拉 Docker 镜像。首次构建慢是 **apk 源 / npm registry** 慢，
> 与这里无关 —— 实测 `node:20-alpine` 是 `CACHED`（1.8s 完成），慢的是装 ffmpeg 那 100 个 apk 包。
> 构建加速见 §6.2。

---

## 4. 建外部网络 `r1net`

```bash
"$PL" -ssh -P 9922 -pw "$PW" -hostkey "$HK" -batch "$HOST" \
  "docker network create r1net 2>/dev/null || docker network inspect r1net --format '{{.Name}} OK'"
```

---

## 5. 部署落雪 lxserver（9527）

`/opt/lxserver/docker-compose.yml`：

```yaml
services:
  lxserver:
    image: xcq0607/lxserver:latest
    container_name: lxserver
    restart: unless-stopped
    ports:
      - "9527:9527"
    volumes:
      - ./data:/server/data       # 自定义音源元数据在这里
      - ./logs:/server/logs
      - ./cache:/server/cache
    environment:
      - TZ=Asia/Shanghai
    networks:
      - r1net

networks:
  r1net:
    external: true
```

> 容器内数据目录是 **`/server/data`**（不是 `/app/data`）；镜像自带 `DATA_PATH` / `LOG_PATH` /
> `PORT=9527` / `BIND_IP=0.0.0.0` / `NODE_ENV=production`，compose 里只需设 `TZ`。
> 用 `docker inspect lxserver --format '{{json .Mounts}}'` 可复核三个挂载点。

```bash
cd /opt/lxserver
docker compose pull
docker compose up -d
docker compose ps
```

**装完立刻改后台密码**：`http://<VM_IP>:9527/admin`，默认 `123456`，务必改掉并记牢 —— 后面导入自定义音源的 API 要用它当 `x-frontend-auth` 头。

> 这个地址从 PC 能打开，前提是 §1.1 里 **9527 那条宿主映射已加**；只映射 8100 会打不开后台。

**开机自启**：Alpine 执行 `rc-update add docker default`，配合 `restart: unless-stopped`，VM 重启后自动拉起。

```bash
curl -s --noproxy '*' -o /dev/null -w '%{http_code}\n' http://$VM_IP:9527/
```

---

## 6. 部署 MyXiaoZhi_MediaServer（8100）

### 6.1 用 podroid 适配版 compose

发布包根目录的 compose 是给群晖 NAS 写的（`/volume1/music` 等）。直接把
**`docs/docker-compose.podroid.yml`** 复制成 `/opt/my-media/docker-compose.yml` 即可（§2.4 ③ 已做），
关键内容如下（带完整注释的可直接照抄版就是该文件）：

```yaml
services:
  r1-media:
    build:
      context: .
      args:
        WITH_FFMPEG: "1"                 # AlphaPi 转码必需
        AP_MIRROR: "mirrors.aliyun.com"          # 可选：apk 换源，见 §6.2
        NPM_REGISTRY: "https://registry.npmmirror.com"   # 可选：npm 换源
    image: r1-media:local
    container_name: MyXiaoZhi_MediaServer
    restart: unless-stopped
    ports:
      - "8100:8100"
    volumes:
      - ./files:/app/files       # ★ 账本 / 池 / 标签缓存 / 电台清单 / 播放记录，必须持久化
      - ./music:/music          # VM 上建个空目录即可
      - ./audiobook:/audiobook  # 同上，只放有声书
    environment:
      - TZ=Asia/Shanghai
      - MUSIC_DIR=/music
      - ABL_DIR=/audiobook
      - RADIOS_FILE=/app/files/radios.json
      - AP_TRANSCODE=auto        # 按客户端身份分流；R1 原样、AlphaPi 转码
      - AP_RATE=24000
      - AP_BITS=8
      - AP_CH=1
    user: "0:0"                 # 别删：SSH 建的 files/ 是 root 属主，USER node 会写不了
    networks:
      - r1net

networks:
  r1net:
    external: true
```

关键点：

- `files/` 必须挂卷：`my_lx`、`pool.json`、`local_cache.json`、`media_flags.json`、`quality.json`、
  `radios.json`、`history.json` 全在里面，**重建容器不丢配置与播放记录**。
- `user: "0:0"` 保留；要么用它，要么 `chown -R 1000:1000 ./files`，二选一，否则 Web 保存音源地址 / 开关会静默失败。
- `RADIO_PROXY` 不写 = 设备直连电台源站（实测优于代理）。
- `WITH_FFMPEG=1` 会在 `node:20-alpine`（Alpine 3.23）里 `apk add ffmpeg`。若报找不到包，
  先 `apk update` 或临时改 `WITH_FFMPEG=0`（那样只服务 R1，AlphaPi 侧不出声）。
- 构建只依赖 `music-metadata`（纯 JS），arm64 无需编译工具链；健康检查是 `wget -qO- http://127.0.0.1:8100/api/health`。

### 6.2 ★ 首次构建为什么慢，以及换源

arm64 VM 上第一次构建会装 **100 个 apk 包 + 14 个 npm 包**，实测各阶段：

| 阶段 | 官方源耗时 |
|---|---|
| `FROM node:20-alpine` | CACHED，1.8s |
| `apk add ffmpeg`（100 包） | **约 762s** |
| `npm ci` | 约 80s |
| 导出 / 解包镜像层 | 约 155s / 55s |

在 VM 容器内实测各镜像源拉 `APKINDEX.tar.gz`（524 KB）与 npm 元数据：

| 源 | 结果 |
|---|---|
| apk `dl-cdn.alpinelinux.org` | 4s |
| **apk `mirrors.aliyun.com`** | **2s（可用）** |
| apk 清华 / 中科大 | 这台 VM 上不通（rc=1） |
| npm `registry.npmjs.org` | 8s |
| **npm `registry.npmmirror.com`** | **2s（可用）** |

所以 `docker-compose.podroid.yml` 默认带这两个 build-arg。它们是 **Dockerfile 里默认留空的可选参数**，
删掉两行就回到官方源，NAS 部署完全不受影响。验证换源是否生效：

```bash
docker run --rm -u root r1-media:local sh -c 'cat /etc/apk/repositories; npm config get registry'
# → https://mirrors.aliyun.com/alpine/v3.23/{main,community}
# → https://registry.npmmirror.com
```

> 即使换了源，SLIRP 模拟网络 + 100 个包解压仍要十几分钟，**别以为卡死**。
> 第二次起构建层全部命中缓存，只重 `COPY . .` 几秒即可。

### 6.3 建目录、构建、启动

```bash
mkdir -p /opt/my-media/files /opt/my-media/music /opt/my-media/audiobook
cd /opt/my-media
docker compose build --progress=plain      # 实时输出，别接 | tail（见 §2.1）
docker compose up -d
docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'
docker logs -f --tail=100 MyXiaoZhi_MediaServer
```

★ 容器 `Started` 之后服务还要 **约 20~30 秒** 才监听（加载音源脚本、电台预热）。这期间
`wget` 报 `Connection reset by peer`、PC curl 返回 `000`、状态显示 `health: starting` 都属正常，
看到日志里 `[ap] 转码就绪：ffmpeg 8.0.1 → pcm_u8/24000/1` 就稳了，别急着重装。

---

## 7. 配置「落雪地址」`my_lx`（最关键一步）

Web 方式：浏览器打开 `http://<VM_IP>:8100` → **音源** 页 → 「落雪 docker（lxserver）」填
`http://lxserver:9527` → 保存（保存会自动跑一次五步自检）。open 用户不用填用户名 / Token。

等价的文件 / 接口方式：

```bash
printf 'http://lxserver:9527\n' > /opt/my-media/files/my_lx

curl -s --noproxy '*' -X POST -H 'Content-Type: application/json' \
  -d '{"url":"http://lxserver:9527"}' http://$VM_IP:8100/api/lx
```

**为什么不能填 `http://<VM_IP>:9527`**：SLIRP guest 的容器内没有到宿主 LAN 的回程路由，
两个容器同在 `r1net` 才能按服务名解析。这一步之后也别再在 Web 音源页把地址「顺手改成 IP」。

---

## 8. 导入洛雪自定义音源（可选，在线歌要用）

lxserver 的自定义源走 API，头带后台密码、body 带 `username:"open"`：

```bash
LX=http://$VM_IP:9527
AUTH='x-frontend-auth: <你改过的后台密码>'

curl -s --noproxy '*' -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"username":"open"}' "$LX/api/custom-source/list"

curl -s --noproxy '*' -H "$AUTH" -F 'username=open' \
  -F 'file=@/opt/lxserver/星海音乐源.js' "$LX/api/custom-source/upload"

curl -s --noproxy '*' -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"username":"open","id":"<源 id>","enable":true}' "$LX/api/custom-source/toggle"

curl -s --noproxy '*' -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"username":"open","order":["星海音乐源.js","裤佬SVIP音源.js"]}' \
  "$LX/api/custom-source/reorder"
```

> ⚠️ **不要用 `/api/custom-source/import` 从 URL 直接导入**：容器内下载脚本会失败。
> 正确做法是宿主机 `curl` 下好脚本，再走 `upload`。
> 元数据落盘在 `/opt/lxserver/data/users/source/_open/{sources,order}.json`。
> 音源之间要互补（此前实测星海擅长 kw/wy、裤佬擅长 tx/mg/kg），**按你手上的版本自行验证**，别照搬排序。

---

## 9. 导入网络电台清单（可选）

**格式坑**：曲库 `local.js` 的 `readRadios` / `saveRadios` 只保留 `name` + `url`，会忽略 `on` 字段。
带 `on:false` 的清单整份传进去，停用台也会被全部启用。正确格式是纯数组：

```json
[{"name":"Hit FM","url":"http://..."}, {"name":"中国之声","url":"http://..."}]
```

```bash
python3 -c 'import json; d=json.load(open("/opt/radios.json.source")); a=d["radios"] if isinstance(d,dict) and "radios" in d else d; json.dump(a,open("/opt/radios.array.json","w"),ensure_ascii=False)'

curl -s --noproxy '*' -X POST -H 'Content-Type: application/json' \
  --data-binary @/opt/radios.array.json http://127.0.0.1:8100/api/radios
```

- `POST /api/radios` 是**整体覆盖**、免重启生效；Web 电台编辑页点「保存」也会重写成纯数组（丢 `on`），
  原始带说明的版本单独留档，别拿它直接 POST。
- 加完台不用重启容器，但**设备侧要 `op=reload`** 才会看到新台。
- VM 上通常没有音乐文件，音源页把「本地音乐 / 本地有声书」块关掉、留「在线 / 电台」，属正常配置。

---

## 10. 端到端验证清单

按顺序跑，任何一步红了就停在那一步查：

```bash
BASE=http://$VM_IP:8100

# ① 容器状态：两个都 Up，曲库容器 healthy
docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'

# ② 健康检查：ver=1.6.8、lx 是服务名、ap.ok=true
curl -s --noproxy '*' "$BASE/api/health"

# ③ 五步自检：落雪首页 → 本容器出网 → 搜索 API → 取链 API → 直链验真
curl -s --noproxy '*' "$BASE/api/lx/selftest"

# ④ 落雪地址必须是服务名
cat /opt/my-media/files/my_lx          # → http://lxserver:9527

# ⑤ 转码链路（AlphaPi 用）：ffmpeg 正弦波自检
curl -s --noproxy '*' "$BASE/api/ap/selftest"

# ⑥ 索引 / 设备识别
curl -s --noproxy '*' "$BASE/index.json" | python3 -m json.tool | head -40
curl -s --noproxy '*' "$BASE/api/whoami"

# ⑦ 电台数量 —— ★ /api/radios 返回的是 {"radios":[...]} 对象，不是数组
curl -s --noproxy '*' "$BASE/api/radios" \
  | python3 -c 'import sys,json;print(len(json.load(sys.stdin)["radios"]))'

# ⑧ LAN 侧（从 PC，必须带 --noproxy '*'）—— 依赖 §1.1 的两条宿主端口映射
curl -s --noproxy '*' -o /dev/null -w '8100 %{http_code}\n' "$BASE/"
curl -s --noproxy '*' -o /dev/null -w '9527 %{http_code}\n' http://$VM_IP:9527/
```

> ⑧ 若 `000` / 超时而 VM 内自测正常，是 Podroid 宿主端口映射没加（或加了没重启 VM），
> 不是服务没起来；8100 和 9527 要**分别**各加一条，见 §1.1。

**2026-11-12 实测通过值**：② `ver=1.6.8`、`lx=http://lxserver:9527`、`ap.ok=true`（ffmpeg 8.0.1）；
③ 五步全绿（3640 / 5287 / 1180 / 994 / 453 ms，直链 `HTTP 206 · 4216KB`）；
⑤ `bytes=36000 expect=36000 ratio=1`；⑥ `index.json` → `onlineCount=100`、`radioCount=51`、
`/ap/index.json` 200；⑦ 51；⑧ 8100 / 9527 均 200。刚开机自检前两步耗时偏高属正常，稳定后降到 0.1~0.7s 量级。

---

## 11. 设备侧收尾

**R1 音箱**：控制台 `http://<音箱IP>:8080` → 设置 → **音乐源**，只留一行，保存后重载：

```
http://<VM_IP>:8100/index.json
```

**AlphaPi STEM（小智固件 V2.4.13）**：配网门户 `192.168.4.1`（长按 A 键 2 秒）→ 「我的媒体库」填
**Podroid 宿主的 LAN IP**（端口默认 8100）→ 说「启用我的媒体库」。设备 IP 由 DHCP 分配会变，别写死。
音频走 `/ap/play`，由 VM 里的 ffmpeg 现转成 8bit / 24k 裸 PCM；播放记录落在 `files/history.json`。

> 设备上填的 `<VM_IP>:8100` 要能通，前提是 §1.1 里 **8100 和 9527 两条宿主端口映射都加好**。
> VM 内只负责把端口监听起来，不要在 VM 里改 iptables 或 compose 端口映射去凑。

---

## 12. 运维与坑速查

```bash
cd /opt/my-media && docker compose logs -f --tail=200
cd /opt/my-media && docker compose restart
cd /opt/my-media && docker compose up -d --build      # 改了 server.js / compose / Dockerfile 才需要
cd /opt/lxserver && docker compose pull && docker compose up -d   # 升落雪
docker network inspect r1net                          # 两个容器都在里面
```

只改 `web/index.html` 不用重启（server.js 每次请求现读该文件）。

| 现象 | 原因 / 处理 |
|---|---|
| PC `curl` 一律 502 | 本机 `HTTP_PROXY`，加 `--noproxy '*'` |
| **VM 内自测正常、PC 侧 `000` / 超时** | Podroid 宿主没加端口映射或加了没重启 VM；**8100 和 9527 两条都要**，见 §1.1 |
| **8100 通、9527 不通（后台 `/admin` 打不开）** | 只加了 8100 那条；落雪后台 / 验收命令走 9527，补加规则 |
| 拉 Docker 镜像超时 | `daemon.json` 配三个加速源 |
| 自检偶发全红 | 容器 DNS 5s；`daemon.json` 加 `223.5.5.5 / 119.29.29.29` |
| **首次构建十几分钟 / 以为卡死** | arm64 上 ffmpeg 要装 100 个 apk 包；配 `AP_MIRROR` / `NPM_REGISTRY`，之后靠层缓存 |
| **plink 返回空输出、镜像没变** | 长命令接了 `\| tail` 导致静默断连；改 `--progress=plain` 实时输出 |
| **启动后 `Connection reset by peer` / curl `000`** | 服务还要 20~30 秒才监听，看 `docker logs` 等 `[ap] 转码就绪` |
| 取链 12s 全超时 | 两个容器不在同一网络，或 `my_lx` 填成了宿主 LAN IP |
| Web 保存音源 / 开关无效 | `files/` 属主 root 但容器跑 `USER node`；保留 `user: "0:0"` |
| 升级后电台 / 在线池 / 播放记录全没了 | 换了部署目录却没迁移 `files/`；见 §2.4 |
| 导入音源失败 | 容器内下载不了；改宿主机下载 + `upload` |
| 停用台也被启用 | `on` 字段被忽略，先过滤成纯数组再 POST |
| 设备看不到新电台 / 新开关 | 设备侧 `op=reload` 重新拉一次曲库 |
| 从旧版升级后两个容器抢 8100 | 目录名 / `container_name` 已改为 `MyXiaoZhi_MediaServer`；先 `docker compose down` 旧项目再起新的 |

---

## 附录：与标准 NAS 版的差异

项目根目录 `README.md` 写的是群晖 / 飞牛 NAS 部署。Podroid / arm64 VM 版只多这六处：

1. 挂载目录从 `/volume1/music` 等换成 `/opt/my-media/{files,music,audiobook}`；
2. 多建一个外部网络 `r1net`，两个容器都挂上去；
3. 落雪与曲库同机部署，`my_lx` 因此必须写**服务名** `http://lxserver:9527`；
4. 需要配 Docker 镜像加速 + 容器 DNS（NAS 通常没这两个问题）；
5. 建议给构建传 `AP_MIRROR` / `NPM_REGISTRY` 换 apk / npm 源（Dockerfile 里默认留空，不影响 NAS）；
6. SSH 走宿主转发的 9922；**8100 和 9527 都要在 Podroid 设置里手动加宿主端口映射**（§1.1），
   NAS 部署没有这一层；容器间互访仍走 `r1net` 服务名，不经过映射。

其余（Web 音源页三块开关、默认音质 128k、`/index.json` 契约、接口一览、AlphaPi 转码参数）
与标准版完全一致，细节查根目录 `README.md`。

**本次实测留档**：`/opt/backup_pre_v168.tar.gz`（重建前备份，含旧 `files/` 与落雪音源数据）、
`/opt/deploy_v168.tar.gz`（上传的源码包）；旧目录 `/opt/r1-media/` 保留未删，可随时回退。
