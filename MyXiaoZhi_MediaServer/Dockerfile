# MyXiaoZhi_MediaServer —— R1 / AlphaPi 双设备的媒体服务器（一个容器，一个端口 8100）
#
#   2026-10-17 由两个容器合并而来：
#     · mini-lx   8100（Node）            在线音源：搜索 / 取链 / 随机池
#     · r1-music  8010（Python + Flask + mutagen） 本地音乐库 + 网络电台
#   合并后本地曲库用 Node 重写（local.js，标签读 music-metadata），
#   8010 整个退休 —— R1 的 files/music_index 只写一个地址就够。
#
#   v1.4.0（2026-11-11）：加 ffmpeg + transcode.js，按客户端身份分流：
#     · R1：原样转发 mp3/m4a（它自带解码器）
#     · AlphaPi（小智）：现转成 WAV 8bit/24000Hz/单声道再推流
#       （小智固件没有 mp3 解码器，flac 更不行；协控通路 1 字节 = 1 样本）
#
#   v1.6.8（2026-11-12）：项目更名 MyXiaoZhi_MediaServer；按设备 MAC 的播放记录、
#     有声书续播与断电兜底、集尾断点夹值 dur-3。
#     权威版本号 = server.js 的 MS_VER（/api/health 返回的 ver）。
#
# 构建：  docker build -t r1-media:local .
# 运行：  docker compose up -d --build
FROM node:20-alpine

# busybox wget 用于 healthcheck；tzdata 让日志时间对（可选）
# ★ v1.4.0：ffmpeg 是 AlphaPi 兼容层的心脏 —— 小智固件没有 mp3 解码器，
#   mp3/m4a/flac 全靠这里现解码 + 重采样到 24kHz 8bit 单声道。
#   只给 R1 用、不想要这 ~60MB 的，可以 `docker build --build-arg WITH_FFMPEG=0`.
#
# v1.6.8 可选加速：arm64 VM / 国内网络首次构建装 ffmpeg 要拉 100 个 apk 包，
#   官方源 + npmjs 会拖到十几分钟。传 AP_MIRROR / NPM_REGISTRY 换源即可，
#   两个参数留空 = 官方源，行为与旧版完全一致（NAS 部署不用改）。
#   AP_MIRROR 只写主机名，如 mirrors.aliyun.com（路径 /alpine/vX.YY/... 与官方相同）。
ARG WITH_FFMPEG=1
ARG AP_MIRROR=""
RUN if [ -n "$AP_MIRROR" ]; then \
      sed -i "s|https://dl-cdn.alpinelinux.org|https://$AP_MIRROR|g" /etc/apk/repositories; \
    fi \
 && apk add --no-cache wget tzdata \
 && if [ "$WITH_FFMPEG" = "1" ]; then apk add --no-cache ffmpeg; fi

WORKDIR /app

# 只有 music-metadata 一个依赖（读本地音频内嵌标签；纯 JS，不需要编译工具链）
COPY package.json package-lock.json* ./
ARG NPM_REGISTRY=""
RUN if [ -n "$NPM_REGISTRY" ]; then npm config set registry "$NPM_REGISTRY"; fi \
 && (npm ci --omit=dev --no-audit --no-fund 2>/dev/null || npm i --omit=dev --no-audit --no-fund)

COPY . .

# files/ 是运行时目录（flag 账本 / pool.json / 本地曲库标签缓存 / radios.json），
# compose 里挂卷持久化
RUN mkdir -p /app/files && chown -R node:node /app/files

USER node

EXPOSE 8100

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8100/api/health >/dev/null 2>&1 || exit 1

# 端口与 NAS 上现有部署对齐（8100）；换端口改这里 + compose 两处
CMD ["node", "server.js", "--port", "8100"]
