'use strict';
/**
 * radioproxy.js —— 网络电台直播流代理（持久上游连接池 + 每客户端水库）
 *
 * ╔════════════════════════════════════════════════════════════════════════╗
 * ║ 三个版本的演进 —— 别把历史改回去                                        ║
 * ╚════════════════════════════════════════════════════════════════════════╝
 *
 * 【v1】直通管道 + 背压传导（失败）
 *      res.write() 返回 false 就 up.pause()。
 *      → 设备停止 read，NAS 就停止从电台收数据
 *      → 电台 CDN 判定慢客户端，掐断
 *      → 重连 + 重新攒 64KB ⇒ **播 6 秒 + 停 3 秒**，每个台精确复现。
 *
 * 【v2】水库：上游全速读，绝不被下游背压掐住（部分成功）
 *      停顿消失了（水库有货），但症状变成 **「同一句 6 秒内容无限循环」**。
 *
 * 【v3】★ 本版：上游连接**池化**，与客户端生命周期解耦（正解）
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ 为什么 v2 会「同一句无限循环」——这是本模块最反直觉的一条坑            │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * 真机日志（`/proc/net/tcp` + logcat）给出了三件 quantitatively 的事实：
 *
 *   ① 设备侧的取流器是 Android 5.1 的 **NuCachedSource2**，日志里反复出现
 *        `I/NuCachedSource2: Cache full, done prefetching for now`
 *      它的策略就是 预取 → 缓存满 → 停止读 → 消耗 → 见底 → 再预取，
 *      并且**在缓存长期不动作时会直接断开 HTTP 连接，重新发起一次请求**。
 *
 *   ② 抓表时设备的 TCP 表里**几乎抓不到到 NAS 的 ESTABLISHED 连接**，
 *      说明这条 HTTP 流的生命周期只有几秒 —— 设备在反复重连代理。
 *
 *   ③ 电台 CDN 对每个**新连接**都会先吐一段开场缓冲（约 6 秒内容，
 *      通常是台标/垫乐/广告），然后才接着推实时流。这段对同一时刻的
 *      **所有新连接都是同一段字节**。
 *
 *   ⇒ 设备每重连一次，代理就新建一条到源的连接，源就再吐一遍同一段开场
 *   ⇒ ON/OFF  +  每次新的连接重复开场
 *   ⇒ 听感就是「同一句话不停重复」。
 *
 *   注意 ①② 是**设备侧的固件行为，改不了**。能改的只有一件事：
 *   **让「客户端重连」不再等于「源那边新建连接」**。
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ 结构（本版）                                                          │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 *         ┌──────────────────────────────────────────────┐
 *         │  Upstream（每个电台 URL 一个全局单例）        │
 *         │  · 到源**只有一条**连接，长期不断             │
 *         │  · 断了自己重连， substituents 无感           │
 *         │  · ring buffer 留最近一段给新来的秒开         │
 *         └─────────┬────────────────────┬───────────────┘
 *                   │ data               │ data
 *              ┌────▼─────┐        ┌─────▼────┐
 *              │ Sub A    │        │ Sub B    │     每个客户端一个水库
 *              │ 自己的   │        │ 自己的   │     背压互不影响，
 *              │ 环形库存 │        │ 环形库存 │     也**不影响上游**
 *              └────┬─────┘        └─────┬────┘
 *                   │ pump               │ pump
 *                R1-1                  R1-2
 *
 *   客户端爱断就断，爱重连就重连 —— **到源那条连接从头到尾就一条**，
 *   源永远不会有第二次「开场」，循环现象因此从根上消失。
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ 【v4】★ 本版修的是「播 6 秒 + 停 3 秒」（2026-10-18 现场定量取证）     │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * 真机三份证据把嫌疑彻底洗清 / 落实：
 *
 *   ① 代理是不是有罪？—— 同一时刻并行抓「直连源站」和「走代理」的字节时序，
 *      **两条曲线逐字节一致**（30 秒各 295.3KB，抖动形态一模一样），
 *      ⇒ 代理是透明的，制造停顿的不是它。
 *
 *   ② 停顿时数据到没到？—— 每 200ms 采样设备侧那条 socket：
 *      /proc/net/tcp6 全程 ESTABLISHED，`rx_queue` **恒为 0**，
 *      wlan0 累计 rx 稳定 ~11.7KB/s（电台占 8KB/s）。
 *      ⇒ 服务端一直在给，设备也一直在积极 read，**网络没有断流**。
 *
 *   ③ 那到底谁喊饿？—— logcat 里设备自己说的话：
 *        I/NuPlayer      : buffer low, pausing...
 *        E/NuPlayerRenderer: now paused audio queue has 4 entries
 *        I/NetPlayer4    : HandleMediaInfoBufferingStart → PlayState 6
 *        （正好 3 秒后）
 *        I/NuPlayer      : buffer ready, resuming...
 *      ⇒ Mediaserver 的渲染队列见底，设备**自己按了暂停**（UI 因此显示暂停态）。
 *
 *   根因 = **零余量（zero slack）**：
 *      源给的是 64kbps 实时流 ⇒ 供给速率（8KB/s）**严格等于**播放速率。
 *      任何前瞻缓冲都只能来自「连接建立瞬间源吐的那段开场爆量」（实测首 250ms
 *      就爆 65KB ≈ 8 秒内容）。这段播完，设备就再也没有余量了，
 *      任何抖动都会让 NuPlayer 的 audio queue 见底 ⇒ 暂停 3 秒 ⇒ 再攒一点 ⇒ 循环。
 *      这也是为什么「每个台都精确复现 6 秒 + 3 秒」。
 *
 *   对症的办法只有一条：**在点播那一刻给设备一大坨**，让它一次吃饱。
 *      冷启动（的 upstream 是刚建的）⇒ 只能拿到源的那 65KB ⇒ 卡。
 *      热启动（upstream 已经跑了一阵 ⇒ ring 里存着几十秒）⇒ 一次倾倒 130KB
 *      ⇒ 实测同一条链路 45 秒 **一次 buffering 都没有**。
 *
 *   ⇒ v4 做三件事：
 *        A. upstream **常热**（pin）：不再「最后一个听众走后 60 秒就关」。
 *        B. ring 放大到 RADIO_RING（默认 512KB ≈ 64 秒内容），给新订阅者一次吃饱。
 *        C. 启动时**预热**前 RADIO_WARM_N 个电台 + LRU 保温用户听过的台。
 *        D. 首吐前至少憋 FIRST_MS（默认 3 秒），让冷启动也能多攒一点。
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ 仍然必须守住的几条底线                                                │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * 1) `Icy-MetaData: 0` —— 请求元数据会让服务端每 metaint 字节插入一段非音频
 *    数据（实测 meta 间隔 16000B @64kbps = 每 2 秒），听感就是规律地卡一下。
 *
 * 2) **客户端没走，就绝不 res.end()**。
 *    v1/v2 都是重连 N 次失败后 `res.end()`，设备一看到流结束就判定
 *    「这首播完了」，playlist 里只有电台一条 ⇒ 立刻从头重播
 *    ⇒ 又一次新连接 ⇒ 又一次同一段开场。这是「循环」的最后一块拼图。
 *    本版：天线 presenting MAX_RETRY 拉到近乎无限，**只要 socket 还开着就一直重试**。
 *
 * 3) 响应头：`Accept-Ranges: none` + chunked + **绝不给 Content-Length**。
 *
 * ── 不在这个模块里做的事 ──
 *   · 不落盘缓存：直播流不能回放。
 */
const http = require('http');
const https = require('https');

function envInt(name, dflt) {
  const v = parseInt(process.env[name], 10);
  return Number.isFinite(v) && v > 0 ? v : dflt;
}

/** 起播预填充：给新来的订阅者先喂这么多字节（约 8 秒的 64kbps 内容）。 */
const PREBUF = envInt('RADIO_PREBUF', 64 * 1024);
/**
 * ★ PREBUF 攒不满时的兜底放行时间（毫秒）：源再慢，最多让设备等这么久。
 *   见 FIRST_MS —— 两者取大者作为「首吐前最多憋多久」。
 */
const PREBUF_WAIT_MS = envInt('RADIO_PREBUF_WAIT', 3000);
/** ★ v4：首吐前至少憋这么久（毫秒）。冷启动也能凑手：爆量 65KB + 3 秒实时。 */
const FIRST_MS = envInt('RADIO_FIRST_MS', 3000);

/** 单个订阅者的水库上限。超过就**丢弃最老的**（见 Sub.push 注释）。 */
const HIGH_WATER = envInt('RADIO_BUFFER', 256 * 1024);
/** 水位回落到这个值以下才恢复写（滞回，避免 pause/resume 抖动）。 */
const LOW_WATER = envInt('RADIO_BUFFER_LOW', Math.min(128 * 1024, Math.floor(HIGH_WATER / 2)));
/**
 * ★ v5：最后一个订阅者离开后，upstream 保留多久再关（毫秒），默认 5 分钟。
 *
 *   定 5 分钟的理由：够覆盖「切走又切回来 / 暂停去干点别的」
 *   （这种回来是热的，不用再等一次 preroll），又不至于像 v4 的 2 小时那样
 *   **没人听也一直收流**（每常热电台 ≈ 8KB/s ≈ 一夜 0.7GB）。
 *   用户明确不要「为了这点卡顿在服务器上 24h 缓冲电台」⇒ 到点就撤，零流量。
 */
const KEEPALIVE_MS = envInt('RADIO_KEEPALIVE', 300000);
/**
 * ★ v4：upstream ring 的上限（字节）——这是「一次能给设备多少」的天花板。
 *   默认 512KB ≈ 64kbps 下的 64 秒。OLD 版本只有 PREBUF*2 = 128KB，
 *   实测给到 130KB 就已经能让设备不再喊饿，512KB 更宽裕；
 *   代价只是内存（每常热电台一份），不算什么。
 */
const RING_MAX = envInt('RADIO_RING', 512 * 1024);
/** ★ v4：同时保持常热的电台数量。常热 = 后台一直连着源站攒 ring。 */
const WARM_N = envInt('RADIO_WARM_N', 3);
/**
 * ★★ v5：常热总开关，**默认关**。
 *
 *   关掉的理由（用户原话）：「没必要为了这 3 秒空档在服务器上 24h 不停缓冲电台，
 *   不值得」。一个 64kbps 的台就是 8KB/s，常热 3 个 ≈ 24KB/s ≈ 一天 2GB，
 *   而这些流量 99% 的时间没有任何人在听。
 *
 *   ★★ 2026-10-19 用户最终拍板：**不再往服务端加任何「替设备缓冲」的东西**
 *   （常热 / 起播预攒 PREROLL 一律不做，后者曾短暂实现又被撤销）——
 *   「问题在设备端，不要搞预攒了，不要再改 docker」。
 *   ⇒ 本文件保持默认直连 + 无常热，只作为**可选的对比开关**存在。
 *   真要对比就设 `RADIO_WARM=1`（常热）+ `RADIO_PROXY=1`（走代理）。
 */
const WARM_ON = envInt('RADIO_WARM', 0) !== 0;
/**
 * ★★ v5：常热电台「多久没人听就退温」（毫秒，默认 30 分钟）。
 *   退温 = 取消 pin 并**彻底断开**上游 ⇒ 这台此后**零流量**。
 *
 *   为什么要这个：常热的代价是「这台一直在收流」——64kbps 的台就是 8KB/s，
 *   3 个常热 ≈ 24KB/s ≈ 一夜 2GB。绝大多数电台听完就没人管了，
 *   没必要一辈子挂着。30 分钟足够覆盖「切走又切回来」的场景，
 *   超过就当没人要，放掉；下次点播重新热（冷启动一次，之后又是热的）。
 *
 *   注意：**没有「暂停不读」这种折中** —— TCP 一旦还连着，源站就会往这边写，
 *   暂停只是把数据压在内核缓冲里，恢复时照样一股脑过来，平均速率还是 8KB/s。
 *   要真省流量只有断开一条路。
 */
const WARM_IDLE_MS = envInt('RADIO_WARM_IDLE', 1800000);
/**
 * ★★ 上游重连次数。故意给到很大的值（默认 10000 ≈ 永不放弃）。
 *    一旦在这里放弃就要 `res.end()`，而 res.end() 会被设备读成「这首播完了」
 *    ⇒ 立即从头重播 ⇒ 新连接 ⇒ 源再吐一次开场 ⇒ 用户听到同一句循环。
 *    宁可一直重试，也绝不在客户端还活着的时候关闭响应。
 */
const MAX_RETRY = envInt('RADIO_RETRY', 10000);
/** 单次连接上游多久没数据算死（毫秒）。 */
const STALL_MS = envInt('RADIO_STALL', 15000);
/** 连接 / 首字节超时（毫秒）。 */
const CONNECT_MS = envInt('RADIO_CONNECT', 12000);

const UA = 'R1XiaoZhiRadio/1.0';

// ---------------------------------------------------------------- 运行期统计
/** srcUrl -> 指标，给 `/api/radio/stat` 现场诊断用。 */
const STATS = new Map();
function noteStat(srcUrl, k, v) {
  let s = STATS.get(srcUrl);
  if (!s) { s = { hits: 0 }; STATS.set(srcUrl, s); }
  if (k === 'hits') s.hits = (s.hits || 0) + 1;
  else s[k] = v;
  if (STATS.size > 64) STATS.delete(STATS.keys().next().value);
  return s;
}

// ---------------------------------------------------------------- 编码
/** Base64URL（无 padding）—— 要塞在 URL path 里，+/ 都得避开。 */
function encTarget(u) {
  return Buffer.from(String(u), 'utf8').toString('base64')
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function decTarget(s) {
  try {
    const b = String(s).replace(/-/g, '+').replace(/_/g, '/');
    const pad = b.length % 4 ? '='.repeat(4 - (b.length % 4)) : '';
    return Buffer.from(b + pad, 'base64').toString('utf8');
  } catch {
    return '';
  }
}

// ══════════════════════════════════════════════════════ Upstream 连接池
/**
 * 一个电台 URL 对应**唯一**一条上游连接。
 * 客户端（音箱）想连就连、想断就断，上游不随之重建 —— 这是本模块的核心。
 */
class Upstream {
  constructor(srcUrl) {
    this.srcUrl = srcUrl;
    /** @type {Set<Sub>} 当前订阅者 */
    this.subs = new Set();
    /** 最近一段数据（给新订阅者秒开，避免它干等 PREBUF 攒满） */
    this.ring = [];
    this.ringLen = 0;
    this.head = null;          // 上游响应头（content-type 等）
    this.req = null;           // 当前 http.ClientRequest
    this.res = null;           // 当前 IncomingMessage
    this.opening = false;
    this.dead = false;         // 已被 destroy，别再复活
    this.retryN = 0;
    this.idleTimer = null;
    this.paused = false;
    this.lastData = Date.now();
    this.watch = null;         // 停滞看门狗
    this.connTimer = null;
    /**
     * ★★ 常热标记（v4）。true = 就算一个订阅者都没有也保持上游连接 + 攒 ring，
     *    让用户点播那一刻能一次性拿到几十秒的内容。见 setWarm() / touch()。
     */
    this.pin = false;
    this.touched = 0;          // 最近一次被「标记要使用」的时间，给 LRU 淘汰用
    /** ★ v5：最近一次「有人订阅」的时刻。用来算「多久没人听了」⇒ 退温。 */
    this.lastSubAt = Date.now();
    /** ★ v5：累计收了多少字节（上游），用于算实时带宽占用。 */
    this.bytes = 0;
    this.bps = 0;
    this._lastBytes = 0;
    /**
     * ★★ 代号。每次建/断连接都 ++ —— 所有异步回调先比对自己出生时的代号，
     *    对不上就直接返回。没有它的话：'close' 和 'error' 会各触发一次 _fail，
     *    排出两个重试 timer ⇒ 并发 _open() 出两条上游连接 ⇒ 两条都往订阅者吐
     *    数据 ⇒ **速率翻倍**（实测 8 秒收了 183KB = 23KB/s，而源只有 8~12KB/s）。
     */
    this.gen = 0;
  }

  // ---- 订阅管理 ----
  attach(sub) {
    clearTimeout(this.idleTimer);
    this.subs.add(sub);
    this.lastSubAt = Date.now();      // ★ v5：有人了，退温倒计时重置
    sub.up = this;
    if (this.head) sub.onHead(this.head);
    // 先喂一段历史数据（可能不足 PREBUF，够了就让 sub 自己判断）
    if (this.ringLen) {
      const feed = this.ring.slice();
      sub.push(feed, this.ringLen);
    }
    if (!this.opening && !this.dead) this._open();
    return this;
  }

  detach(sub) {
    this.subs.delete(sub);
    this.lastSubAt = Date.now();      // ★ v5：从这一刻开始算「无人时长」
    // ★ v4：常热的连接不跟着最后一个听众走 —— 它要接着攒 ring，
    //   给下一个听众（很可能就是同一个人点重播）留一坨身家。
    if (this.pin) return;
    if (!this.subs.size && !this.dead) {
      // ★ 别立刻关：设备一个 ON/OFF 周期只有几秒，切台/重播也很常见，
      //   立刻关就等于每次都重连源 ⇒ 又听到一次开场。
      clearTimeout(this.idleTimer);
      this.idleTimer = setTimeout(() => this.destroy(), KEEPALIVE_MS);
    }
  }

  destroy() {
    if (this.dead) return;
    this.dead = true;
    clearTimeout(this.idleTimer);
    clearInterval(this.watch);
    clearTimeout(this.connTimer);
    try { if (this.res) this.res.destroy(); } catch { /* ignore */ }
    try { if (this.req) this.req.destroy(); } catch { /* ignore */ }
    UPSTREAMS.delete(this.srcUrl);
  }

  // ---- 上游连接 ----
  _open() {
    if (this.dead || this.opening) return;   // ★ opening 守卫：杜绝并发重开出第二条连接
    this.opening = true;
    const myGen = ++this.gen;

    const mod = /^https:/i.test(this.srcUrl) ? https : http;
    let req;
    try {
      req = mod.request(this.srcUrl, {
        method: 'GET',
        headers: {
          'User-Agent': UA,
          'Accept': 'audio/mpeg,audio/*,*/*',
          'Accept-Encoding': 'identity',  // 直播流 gzip 没意义，还多一道解压
          'Icy-MetaData': '0',            // ★★ 不请求元数据，避免 metaint 插数据
          'Connection': 'close',
        },
        timeout: 0,                       // ★ 关掉 socket idle timeout：直播流就是持续地跑
      });
      // ★★ http.request() 不像 http.get() 会自动 end()，不调就永远发不完整请求。
      req.end();
    } catch (e) {
      console.error('[radio] request build failed: ' + e.message);
      this.opening = false;
      return this._scheduleRetry('build: ' + e.message);
    }

    this.req = req;
    // ★★ 这两个定时器必须先声明为 null：上游可能一直不来 response，
    //    此时 connect timeout 会先触发 _fail()，而 watch 还没到声明行 ——
    //    用 const 写在 'response' 回调里会得到 ReferenceError（TDZ），
    //    且异常从定时器回调抛出会直接把整个进程带走（2026-10-18 实测崩过）。
    clearTimeout(this.connTimer);
    this.connTimer = setTimeout(() => this._fail('connect timeout', myGen), CONNECT_MS);

    req.on('response', (r) => {
      if (this.gen !== myGen) { try { r.destroy(); } catch { /* ignore */ } return; }
      clearTimeout(this.connTimer);
      if (r.statusCode >= 400) {
        r.resume();
        return this._fail('upstream HTTP ' + r.statusCode, myGen);
      }
      this.res = r;
      this.head = r.headers;
      this.retryN = 0;                       // 成功收数据 ⇒ 连败计数清零
      noteStat(this.srcUrl, 'ctype', r.headers['content-type'] || '?');
      noteStat(this.srcUrl, 'metaint', r.headers['icy-metaint'] || 0);
      for (const s of this.subs) s.onHead(r.headers);

      this.lastData = Date.now();
      clearInterval(this.watch);
      this.watch = setInterval(() => {
        if (this.dead || this.gen !== myGen) return clearInterval(this.watch);
        if (Date.now() - this.lastData > STALL_MS) this._fail('stalled ' + STALL_MS + 'ms', myGen);
      }, 2000);

      r.on('data', (chunk) => {
        if (this.dead || this.gen !== myGen) return;
        this.lastData = Date.now();
        this.bytes += chunk.length;
        // 入共享 ring（给后来 slot 的新订阅者秒开）
        this.ring.push(chunk);
        this.ringLen += chunk.length;
        while (this.ringLen > RING_MAX && this.ring.length > 1) {
          this.ringLen -= this.ring.shift().length;
        }
        // 分发给每个订阅者（各自的背压互不影响，也**不影响上游**）
        for (const s of this.subs) s.push([chunk], chunk.length);
      });

      r.on('close', () => this._fail('upstream closed', myGen));
      r.on('error', (e) => this._fail('upstream read: ' + e.message, myGen));
    });

    req.on('error', (e) => this._fail('upstream: ' + e.message, myGen));
  }

  /** @param {number} myGen 发起这条连接时的代号；对不上说明我已经是历史了。 */
  _fail(why, myGen) {
    if (this.dead) return;
    if (myGen !== undefined && myGen !== this.gen) return;   // 旧连接的迟到回调，忽略
    clearTimeout(this.connTimer);
    clearInterval(this.watch);
    try { if (this.res) this.res.destroy(); } catch { /* ignore */ }
    try { if (this.req) this.req.destroy(); } catch { /* ignore */ }
    this.res = null; this.req = null;
    this.opening = false;
    noteStat(this.srcUrl, 'lastFail', why + ' @' + new Date().toISOString().slice(11, 19));
    this._scheduleRetry(why);
  }

  _scheduleRetry(why) {
    if (this.dead) return;
    if (this.retryN >= MAX_RETRY) {
      noteStat(this.srcUrl, 'gaveUp', why);
      return this.destroy();
    }
    this.retryN++;
    const d = Math.min(500 * Math.pow(2, Math.min(this.retryN, 6)), 10000);
    setTimeout(() => this._open(), d);
  }
}

/** @type {Map<string, Upstream>} */
const UPSTREAMS = new Map();
function getUpstream(srcUrl) {
  let u = UPSTREAMS.get(srcUrl);
  if (!u || u.dead) {
    u = new Upstream(srcUrl);
    UPSTREAMS.set(srcUrl, u);
  }
  return u;
}

// ══════════════════════════════════════════════ v4：常热（pre-warm）
/**
 * 常热集合。集合里的电台：一个听众都没有也照样连着源，ring 一直攒着。
 *
 * 为什么值得这么干（见文件头的取证）：
 *   源只有 8KB/s 实时 ⇒ 设备能不能不卡，全看「点播那一刻能一次给它多少」。
 *   冷启动只有 8 秒 ⇒ 必然反复 buffer low；常热之后能一次给几十秒 ⇒ 不卡。
 */
const WARM = new Set();

/** 有没有活着的连接（含正在连的那一瞬）。 */
function isOpen(u) { return !!u && !u.dead && (u.opening || !!u.res); }

/**
 * 标记「这个台要用」：纳入常热 + 没连就立刻连。
 * 每次 `/radio/<x>` 被请求都会调一次 ⇒ 用户听过的台自然留在这份 LRU 里。
 */
function touch(srcUrl) {
  if (!/^https?:\/\//i.test(srcUrl)) return null;
  const u = getUpstream(srcUrl);
  u.touched = Date.now();
  // ★ v5：关了常热就别 pin —— 只保证「现在要用就连上」，用完按 KEEPALIVE 收掉。
  if (!WARM_ON) {
    if (!isOpen(u)) u._open();
    return u;
  }
  if (!WARM.has(srcUrl)) {
    WARM.add(srcUrl);
    while (WARM.size > WARM_N) {
      let oldest = null;
      for (const x of WARM) {
        const o = UPSTREAMS.get(x);
        if (!o) { WARM.delete(x); continue; }        // 已被回收，顺手清账
        if (!oldest || (o.touched || 0) < (oldest.touched || 0)) oldest = o;
      }
      if (!oldest || oldest.srcUrl === srcUrl) break; // 自己也还没更热，别把自己踢了
      WARM.delete(oldest.srcUrl);
      oldest.pin = false;
      if (!oldest.subs.size) {
        // 归还源站资源，但先给 KEEPALIVE_MS 的宽限（用户很可能马上点回来）
        clearTimeout(oldest.idleTimer);
        oldest.idleTimer = setTimeout(() => { if (!oldest.subs.size) oldest.destroy(); }, KEEPALIVE_MS);
      }
    }
  }
  u.pin = true;
  u.touched = Date.now();
  if (!isOpen(u)) u._open();
  return u;
}

/**
 * 预热：把给定的电台列表逐个拉进常热集合（受 WARM_N 约束）。
 * server.js 读电台清单之后调一次即可。
 *
 * @param {Array<string|{url:string}>} urls
 */
function setWarm(urls) {
  if (!WARM_ON) return { warm: 0, touched: 0, wanted: (urls || []).length, off: true };
  let n = 0;
  for (const r of urls || []) {
    const u = (typeof r === 'string') ? r : (r && r.url);
    if (touch(u)) n++;
  }
  return { wanted: (urls || []).length, warm: WARM.size, touched: n };
}

// ─────────────────────────────────────────────────── v5：退温 + 带宽计量
/**
 * 每分钟巡一遍常热池：
 *   · 无人听超过 WARM_IDLE_MS 的 ⇒ 退温（unpin + 断开）⇒ 这台此后零流量。
 *   · 顺手把每台的实时速率算出来，给 `/api/radio/stat` 看「现在占多少带宽」。
 */
let METER = null;
function startMeter() {
  if (METER) return;
  METER = setInterval(() => {
    const now = Date.now();
    for (const [url, u] of UPSTREAMS) {
      // 速率：这一分钟收了多少字节 / 60（顺带汇报 kbps）
      const d = u.bytes - u._lastBytes;
      u._lastBytes = u.bytes;
      u.bps = Math.round(d / 60);
      if (!u.pin || u.dead) continue;
      if (u.subs.size) continue;                    // 有人在听，永不退温
      if (now - u.lastSubAt < WARM_IDLE_MS) continue;
      // 退温
      WARM.delete(url);
      u.pin = false;
      noteStat(url, 'cooled', 'no listener for ' + Math.round((now - u.lastSubAt) / 1000) + 's');
      u.destroy();
    }
  }, 60000);
  if (METER.unref) METER.unref();      // 别拖着进程不退出
}
startMeter();

// ══════════════════════════════════════════════════════ 单个客户端
/**
 * 一个 HTTP 请求 ⇄ 一个 Sub。
 * 自己维护水库，自己的出让节奏由自己的 res 决定，与别人无关。
 */
class Sub {
  constructor(res, srcUrl, onFinish) {
    this.res = res;
    this.srcUrl = srcUrl;
    this.onFinish = onFinish;
    this.up = null;

    this.buf = [];
    this.len = 0;
    this.headSent = false;
    this.started = false;      // 是否已经吐过数据给设备
    this.closed = false;
    this.ended = false;
    this.writeOk = true;
    this.pumping = false;
    this.total = 0;
    this.minBuf = Infinity;
    /** ★ v4：出生时刻 —— 首吐前要憋够 FIRST_MS，见 canStart()。 */
    this.born = Date.now();
    this.noteT = 0;
  }

  /**
   * 能不能吐第一个字节。
   *
   * 攒够 PREBUF 当然最好；攒不够也不急着吐 —— **早吐不如多等一会儿**。
   * 因为设备感受不到 "第 8 秒能出声" 和 "第 11 秒能出声" 的差别，
   * 但能感受到 "听 6 秒卡 3 秒"：那 3 秒等单位換来的几十 KB，
   * 就是它后面一路不喊饿的本钱。
   */
  canStart() {
    if (this.len >= PREBUF) return true;
    return Date.now() - this.born >= FIRST_MS;
  }

  onHead(h) {
    if (this.headSent) return;
    this.headSent = true;
    try {
      this.res.writeHead(200, {
        'Content-Type': h && h['content-type'] || 'audio/mpeg',
        'Cache-Control': 'no-cache, no-store',
        'Pragma': 'no-cache',
        'Expires': '0',
        'Accept-Ranges': 'none',   // 直播，别让播放器去拖进度
        'Connection': 'keep-alive',
        // 直播流**绝不能**给 Content-Length
      });
      if (typeof this.res.flushHeaders === 'function') this.res.flushHeaders();
    } catch { this.ended = true; }
  }

  /** 上游数据到达：入库，然后尝试泵给设备。 */
  push(chunks, nbytes) {
    if (this.closed || this.ended) return;
    for (const c of chunks) this.buf.push(c);
    this.len += nbytes;
    this.total += nbytes;
    // ★ 水库封顶：设备长时间不 read（比如被切到后台但 socket 还开着）时，
    //   不封顶就会把 NAS 内存吃干。直播流里过期的数据本就没用了，
    //   丢最老的比无上限增长划算。正常 ON/OFF 周期只积压几十 KB，
    //   离 256KB 差得远，永远不会触发。
    while (this.len > HIGH_WATER && this.buf.length > 1) {
      this.len -= this.buf.shift().length;
    }
    // 起播门槛：攒够 PREBUF 才吐；冷启动攒不够时，憋到 FIRST_MS 也让过。
    if (!this.started && !this.canStart()) return;
    // ★ 会话中的轻量指标（/api/radio/stat 现场看活着的会话）
    if (Date.now() - this.noteT > 1000) {
      this.noteT = Date.now();
      noteStat(this.srcUrl, 'liveBytes', this.total);
      noteStat(this.srcUrl, 'liveSec', Math.round((Date.now() - this.born) / 1000));
    }
    this.pump();
  }

  /**
   * 把水库里的东西吐给设备。
   *
   * ★★★ 本模块最容易写错、也是最难查的一个点 ★★★
   * Node 的 `res.write()` **从不拒绝数据**：它一定会被塞进输出队列；
   * 返回 false 只是提示「内部缓冲超过 highWaterMark 了，建议先歇会儿」。
   *
   * 所以**必须无条件清空水库**。写成
   *
   *     if (!this.safeWrite(out)) break;      ✗ 错
   *     this.buf = [];
   *
   * 的后果是：每次 write 都返回 false（我们一次就是 64KB，远超默认 16KB
   * 阈值，所以**必然**返回 false）⇒ 每次都在这儿 break ⇒ 水库永远不清
   * ⇒ 下一次 'drain' 又把同一份数据再写一遍
   * ⇒ **设备反复听到同几秒钟内容**（2026-10-18 用户报的「同一句话不停
   *   重复」的真正根因，v1/v2/v3 全都栽在这里，实测 12 秒能吐 12.5GB）。
   */
  pump() {
    if (this.closed || this.ended || this.pumping) return;
    this.pumping = true;
    try {
      while (this.buf.length) {
        const out = this.buf.length === 1 ? this.buf[0] : Buffer.concat(this.buf);
        const ok = this.safeWrite(out);
        // ↓↓↓ 无论 ok 与否，数据都已经入队了 —— 水库必须扣账，绝不能重发。
        this.buf = [];
        this.len -= out.length;
        if (this.len < 0) this.len = 0;
        this.started = true;
        if (!ok) { this.writeOk = false; break; }   // 歇会儿，等 'drain'
      }
    } finally {
      this.pumping = false;
    }
    if (this.len < this.minBuf) this.minBuf = this.len;
  }

  safeWrite(b) {
    if (this.closed || this.ended) return false;
    try { return this.res.write(b); } catch { this.ended = true; return false; }
  }

  drain() { this.writeOk = true; this.pump(); }

  finish(why) {
    if (this.closed) return;
    this.closed = true;
    clearTimeout(this.startTimer);
    noteStat(this.srcUrl, 'lastMinBuf', this.minBuf === Infinity ? 0 : this.minBuf);
    noteStat(this.srcUrl, 'bytes', this.total);
    if (this.up) this.up.detach(this);
    // ★ 主动 end 只在客户端真的走了之后；正常路径走不到这里。
    try { if (!this.ended) { this.ended = true; this.res.end(); } } catch { /* ignore */ }
    if (this.onFinish) this.onFinish();
  }
}

// ══════════════════════════════════════════════════════ 对外主控
/**
 * 处理一次 `GET /radio/<b64url>`。
 *
 * @param {string} b64      path 里那段 base64url
 * @param {object} req      Node IncomingMessage
 * @param {object} res      Node ServerResponse
 * @param {string} selfUrl  本站对外地址，用于套娃防御
 */
function serve(b64, req, res, selfUrl) {
  const srcUrl = decTarget(b64);

  if (!/^https?:\/\//i.test(srcUrl)) {
    return fail(res, 400, 'bad target scheme: ' + JSON.stringify(srcUrl).slice(0, 80));
  }
  // ★ 套娃防御：Web 的电台编辑页拿 /api/radios 的**原始 URL** 回写；
  //   万一被改成了代理地址，在这里掐断，而不是一层套一层直到 fd 耗尽。
  if (selfUrl && srcUrl.toLowerCase().indexOf(String(selfUrl).toLowerCase()) === 0) {
    return fail(res, 400, 'refusing to proxy itself');
  }

  // ★ v4：把这个台拉进常热 LRU 并确保上游是热的（冷的也立刻开始加热）。
  const up = touch(srcUrl);

  const sub = new Sub(res, srcUrl);
  noteStat(srcUrl, 'hits', 1);

  res.on('close', () => sub.finish('client close'));
  res.on('drain', () => sub.drain());

  (up || getUpstream(srcUrl)).attach(sub);

  // ★ 启播兜底：源特别慢时也不让设备干等，到点就把攒到的东西吐出去。
  sub.startTimer = setTimeout(() => {
    if (!sub.started && sub.len) sub.pump();
  }, Math.max(PREBUF_WAIT_MS, FIRST_MS));

  return sub;
}

function fail(res, code, msg) {
  try {
    res.writeHead(code, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('radio proxy: ' + msg);
  } catch { /* ignore */ }
}

module.exports = {
  PREBUF, PREBUF_WAIT_MS, FIRST_MS, RING_MAX, WARM_N, WARM_ON, WARM_IDLE_MS,
  HIGH_WATER, LOW_WATER, MAX_RETRY,
  STALL_MS, CONNECT_MS, KEEPALIVE_MS,
  encTarget, decTarget, serve,
  setWarm, touch,
  /** 常热集合现状，给 /api/radio/stat 看。 */
  warmInfo: () => ({
    on: WARM_ON, n: WARM.size, max: WARM_N, idleMs: WARM_IDLE_MS,
    list: Array.from(WARM).map((u) => {
      const o = UPSTREAMS.get(u);
      return {
        url: u, open: isOpen(o), ringLen: o ? o.ringLen : 0,
        bps: o ? o.bps : 0, kbps: o ? +(o.bps * 8 / 1000).toFixed(1) : 0,
        noSubSec: o ? Math.round((Date.now() - (o.lastSubAt || 0)) / 1000) : -1,
        /** 还有多少秒就退温（没人听的话）。 */
        coolInSec: o ? Math.max(0, Math.round((WARM_IDLE_MS - (Date.now() - (o.lastSubAt || 0))) / 1000)) : -1,
      };
    }),
  }),
  /** 现场诊断：每个电台的上游状态 + 最近一次会话的量化指标。 */
  stats: () => {
    const o = {};
    for (const [k, v] of STATS) o[k] = Object.assign({}, v);
    let totBps = 0;
    for (const [url, u] of UPSTREAMS) {
      totBps += u.bps || 0;
      o[url] = Object.assign(o[url] || {}, {
        live: !u.dead, subs: u.subs.size, retryN: u.retryN,
        ringLen: u.ringLen, pin: !!u.pin, open: isOpen(u),
        bps: u.bps, kbps: +(u.bps * 8 / 1000).toFixed(1),
        noSubSec: Math.round((Date.now() - (u.lastSubAt || 0)) / 1000),
        bytes: u.bytes,
      });
    }
    /** 汇总：现在所有上游加起来占多少带宽（只在 /api/radio/stat 顶层出现一次）。 */
    o.__total = {
      upstreams: UPSTREAMS.size, warm: WARM.size, warmOn: WARM_ON,
      bps: totBps, kbps: +(totBps * 8 / 1000).toFixed(1),
      /** 按这个速率跑一整天会吃掉多少 MB（给用户判断要不要调 WARM_N）。 */
      mbPerDay: +(totBps * 86400 / 1048576).toFixed(0),
    };
    return o;
  },
};
