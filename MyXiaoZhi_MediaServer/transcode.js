'use strict';
/**
 * transcode.js —— AlphaPi 兼容层：任意音频 → 设备能直接吃的 PCM
 *
 * <p>为什么需要这一层：R1 的播放器（NuCachedSource2 + MP3/AAC 解码）能直接吃
 * 在线音源给的 mp3/m4a；AlphaPi 那边的音频链路是「N32 协控 + 1 字节 = 1 样本」
 * 的老式通路 —— 它只认 **8bit / 24000Hz / 单声道** 的裸采样（外面包不包 44 字节
 * WAV 头都行，协控根本不读头里的 bits/rate）。把 mp3 直接丢过去就是一片白噪。
 *
 * <p>所以：识别到客户端是 AlphaPi，就在这里用 ffmpeg 现解码 + 重采样 + 位深降到 8bit，
 * 再把 PCM 以 WAV（默认）或裸流（raw=1）推出去。R1 那条路一个字节都不变。
 *
 * <p>★ 三个不能踩的坑（都吃过）：
 *   ① **不要给流式响应写真实长度**。ffmpeg 的 `-f wav` 管道输出会把 RIFF/data
 *      长度写成 0xFFFFFFFF，某些解析器按它算剩余长度会直接放弃；我们自己拼 44 字节
 *      头（同样写 0xFFFFFFFF）更稳 —— 这也是 AlphaPi 对讲机/收音机固件在用的写法。
 *   ② **必须等第一块数据到了再 writeHead**。源站 404 / 没有音轨时 ffmpeg 秒退，
 *      先发 200 就只能给客户端一个「200 + 空响应」，设备侧表现为「点了没反应」。
 *   ③ **必须给 ffmpeg 挂满清理**。ESP32 端拖进度/切歌就是直接掐 TCP，
 *      不 kill 子进程的话每切一次歌就漏一个 ffmpeg，NAS 上很快堆成几十个。
 */

const { spawn } = require('child_process');

const FFMPEG = process.env.FFMPEG_PATH || 'ffmpeg';

/** 输出参数（AlphaPi 侧固定组合；要试别的档位用环境变量，不用改代码） */
const OUT = {
  rate: clampInt(process.env.AP_RATE, 24000, 8000, 48000),
  bits: parseInt(process.env.AP_BITS, 10) === 16 ? 16 : 8,
  ch: clampInt(process.env.AP_CH, 1, 1, 2),
  gainDb: Number(process.env.AP_GAIN_DB || 0),
};
if (!Number.isFinite(OUT.gainDb)) OUT.gainDb = 0;

/** 首字节超时（源站连不上 / 没有音轨时别让设备干等） */
const START_TIMEOUT = clampInt(process.env.AP_START_TIMEOUT_MS, 20000, 3000, 120000);
/** 传输期空闲超时：这么久一个字节都没出来就掐掉（网络源卡死 / 上游断流） */
const IDLE_TIMEOUT = clampInt(process.env.AP_IDLE_TIMEOUT_MS, 20000, 5000, 120000);

function clampInt(v, d, lo, hi) {
  const n = parseInt(v, 10);
  if (!Number.isFinite(n)) return d;
  return Math.min(Math.max(n, lo), hi);
}

/* ★ v1.6.6 优化②：按设备实际消耗速率匀速发送，服务端常握 1~2s 存货。
   旧逻辑是"ffmpeg 出一个 chunk 就 res.write 一个"，靠 TCP 背压 pause/resume 把速率
   钳在实时；但 pause/resume 的粒度是整块 chunk，一停一放就是脉冲式批量到货，设备
   32KB FIFO 一抖就抽干。现在每 PACE_TICK_MS 发固定字节（= bps × 20ms），先攒
   PACE_PREBUF_MS 再开闸，队列上限 PACE_MAXQ_MS；ffmpeg 被队列背压 pause 住，
   服务端始终有一两秒存货，网络重传时设备不会立刻断粮。
   AP_PACE=0 一键回退到老的直写+背压逻辑。 */
const PACE_ON = process.env.AP_PACE !== '0';
const PACE_TICK_MS = clampInt(process.env.AP_PACE_TICK_MS, 20, 5, 100);
const PACE_PREBUF_MS = clampInt(process.env.AP_PREBUF_MS, 200, 0, 5000);
const PACE_MAXQ_MS = clampInt(process.env.AP_MAXQ_MS, 2000, 500, 10000);
/* ★ v1.6.7：下游（设备）连续这么久一个字节都不读才放弃。与「上游空闲」分开计：
   设备起播预滚 / 自动续播时可能短暂不读 socket，旧代码把这两种情况混在一起，
   20s 空闲看门狗就把 ffmpeg SIGKILL 了（实机第 8 集只发 480B 就断，
   history 里 ts→endTs 精确 20068ms）。AP_DOWN_TIMEOUT_MS 可调。 */
const DOWN_TIMEOUT_MS = clampInt(process.env.AP_DOWN_TIMEOUT_MS, 60000, 10000, 300000);

/**
 * ffmpeg 的英文报错 → 一句能直接指导下一步的中文。
 * 设备侧只看到「点了没声音」时，日志里这句话就是唯一线索，
 * 别再让用户去猜 `Invalid data found when processing input` 是什么意思。
 */
function explainErr(raw) {
  const s = String(raw || '');
  if (/Invalid data found|moov atom not found|could not find codec parameters/i.test(s))
    return '拿到的不是可解码的音频（常见于取链给回网页/JSON/加密地址）';
  if (/Server returned 40[34]|40[34] (Forbidden|Not Found)/i.test(s))
    return '音源拒绝访问（403/404：链过期或被防盗链拦了）';
  if (/timed out|Timeout|Operation timed out|Connection reset/i.test(s))
    return '连不上音源（超时/被重置）';
  if (/does not contain any stream|matches no streams|Output file .* empty/i.test(s))
    return '这个文件里没有音频轨';
  if (/No such file or directory/i.test(s))
    return '文件不存在（本地库路径变了？）';
  if (/Conversion failed/i.test(s)) return '解码/重采样失败';
  return s.replace(/\s+/g, ' ').trim().slice(0, 160) || '未知原因';
}


// ---------------------------------------------------------------- ffmpeg 探测

/** null = 还没探测过；{ok,ver,err,at} = 探测结果 */
let probe = null;
let probing = null;

function state() {
  return probe || { ok: false, ver: '', err: '尚未探测', at: 0 };
}

/**
 * 探测 ffmpeg 是否可用（启动时跑一次，之后长期缓存）。
 * ★ 不能用 `which`：容器里 PATH 可能只有 /usr/bin，`ffmpeg` 却装在 /usr/local/bin。
 *   直接 `ffmpeg -version`，能跑就算有。
 */
function check() {
  if (probing) return probing;
  probing = new Promise(resolve => {
    let out = '', err = '';
    let done = false;
    const fin = r => {
      if (done) return;
      done = true;
      probe = Object.assign({ at: Date.now() }, r);
      probing = null;
      resolve(probe);
    };
    let child;
    try {
      child = spawn(FFMPEG, ['-hide_banner', '-version'], { stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
      return fin({ ok: false, ver: '', err: String(e && e.message || e) });
    }
    child.on('error', e => fin({ ok: false, ver: '', err: String(e && e.message || e) }));
    child.stdout.on('data', d => { out += d.toString('utf8'); });
    child.stderr.on('data', d => { err += d.toString('utf8'); });
    child.on('close', code => {
      if (code === 0 && /ffmpeg version/i.test(out)) {
        const m = /ffmpeg version\s+([^\s]+)/i.exec(out);
        return fin({ ok: true, ver: m ? m[1] : 'unknown', err: '' });
      }
      fin({ ok: false, ver: '', err: (err || out || ('exit ' + code)).slice(0, 300) });
    });
    // 兜底：探测本身不许挂住启动流程
    setTimeout(() => fin({ ok: false, ver: '', err: '探测超时' }), 8000).unref();
  });
  return probing;
}

// ---------------------------------------------------------------- WAV 头

/**
 * 44 字节流式 WAV 头。
 * 长度字段一律写 0xFFFFFFFF（= 未知长度/持续到连接结束）——
 * 这是 AlphaPi 对讲机、STEM 收音机固件与协控约定的写法，两边都认。
 */
function wavHeader(rate, bits, ch) {
  const blockAlign = Math.max(1, (ch * bits) >> 3);   // 8bit 单声道 = 1
  const byteRate = rate * blockAlign;
  const b = Buffer.alloc(44);
  b.write('RIFF', 0, 'ascii');
  b.writeUInt32LE(0xFFFFFFFF, 4);
  b.write('WAVE', 8, 'ascii');
  b.write('fmt ', 12, 'ascii');
  b.writeUInt32LE(16, 16);            // PCM fmt 块长度（无 cbSize）
  b.writeUInt16LE(1, 20);             // format = 1 (PCM)
  b.writeUInt16LE(ch, 22);
  b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(byteRate, 28);
  b.writeUInt16LE(blockAlign, 32);
  b.writeUInt16LE(bits, 34);
  b.write('data', 36, 'ascii');
  b.writeUInt32LE(0xFFFFFFFF, 40);
  return b;
}

// ---------------------------------------------------------------- 参数拼装

/**
 * 拼 ffmpeg 参数。
 *
 * @param {Array<{input:string,kind:'file'|'url',ss?:number}>} items
 *   一条 = 单曲/电台（单输入，行为与 v1.4.0 完全一致）；
 *   多条 = **歌单连播**（v1.5.1）：每路先 aresample 到统一格式再 concat，
 *   一首接一首**在同一条 HTTP 流里**送出去 —— 设备端只当"一首长音频"，
 *   不用自己队列、不用二次取流，切歌缝在 NAS 侧完成。
 *   `ss`（秒）= 输入侧跳转（v1.6.0 有声书续播用）：只对给了 ss 的那一路生效。
 *
 * ★ concat filter 要求各输入采样格式/率/声道一致，所以每路都过
 *   `aresample + aformat`；只在输出统一格式会报 "Input channels are not
 *   matching" 或者直接拼出半速噪声（这坑 TTS 那边踩过）。
 */
function buildArgs(items, o) {
  const N = items.length;
  const a = ['-hide_banner', '-nostdin', '-loglevel', 'error'];
  for (const it of items) {
    if (it.kind === 'url' && /^https?:/i.test(it.input)) {
      // 上游超时 / 重连 / UA：不少音源 CDN 会掐默认 UA，卡住的连接也要能自己断
      a.push('-rw_timeout', '15000000',
        '-user_agent', 'Mozilla/5.0',
        '-reconnect', '1', '-reconnect_streamed', '1', '-reconnect_delay_max', '5',
        '-icy', '0');
    }
    // ★ v1.6.0：输入侧跳转（有声书续播）。放在 -i **前面** = 快速 seek，
    //   本地文件直接按关键帧定位，远端 URL 也能靠 Range 少下一大截。
    //   ★ 不能写成输出侧 -ss：那样 ffmpeg 仍要从头解码，25 分钟的集子白解一遍。
    const ss = Number(it.ss);
    if (Number.isFinite(ss) && ss > 0.2) a.push('-ss', ss.toFixed(2));
    a.push('-i', it.input);
  }
  const gain = Number(o.gainDb) || 0;
  if (N === 1) {
    a.push('-vn', '-map', '0:a:0');
    if (gain) a.push('-af', 'volume=' + gain + 'dB');
  } else {
    const lay = o.ch === 2 ? 'stereo' : 'mono';
    const parts = items.map((_, i) =>
      `[${i}:a]aresample=${o.rate},aformat=sample_fmts=fltp:sample_rates=${o.rate}:channel_layouts=${lay}[a${i}]`);
    const cat = items.map((_, i) => `[a${i}]`).join('')
      + `concat=n=${N}:v=0:a=1[cat]`;
    const tail = gain ? `[cat]volume=${gain}dB[out]` : '[cat]anull[out]';
    a.push('-filter_complex', parts.join(';') + ';' + cat + ';' + tail, '-map', '[out]');
  }
  a.push('-ac', String(o.ch), '-ar', String(o.rate),
    '-acodec', o.bits === 16 ? 'pcm_s16le' : 'pcm_u8',
    '-f', o.bits === 16 ? 's16le' : 'u8',
    'pipe:1');
  return a;
}

// ---------------------------------------------------------------- 主流程

/**
 * 把 input（本地文件 / 远端 URL）转成设备格式直接推进 res。
 *
 * @param {object} req
 * @param {object} res
 * @param {object} opt
 *   input  {string} 本地绝对路径或 http(s) 地址
 *   kind   {'file'|'url'}
 *   raw    {boolean} true = 不带 WAV 头（协控直喂用）；默认 false
 *   tag    {string}  日志前缀（歌名之类，纯排障用）
 * @returns {Promise<'done'|'no-ffmpeg'|'failed'>}
 */
function serve(req, res, opt) {
  const raw = !!opt.raw;
  // ★ v1.5.1：items = 一条流里要连续送出的曲目（单曲/电台就是一条）
  const items = (Array.isArray(opt.items) && opt.items.length ? opt.items : [{ input: opt.input, kind: opt.kind }])
    .map(it => ({
      input: String((it && it.input) || ''),
      kind: (it && it.kind) === 'url' ? 'url' : 'file',
      ss: Number(it && it.ss) > 0 ? Number(it.ss) : 0,
    }))
    .filter(it => it.input);
  if (!items.length) {
    res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('没有可转码的音源（items 为空）');
    return Promise.resolve('failed');
  }
  const o = {
    rate: clampInt(opt.rate, OUT.rate, 8000, 48000),
    bits: opt.bits === 16 ? 16 : OUT.bits,
    ch: clampInt(opt.ch, OUT.ch, 1, 2),
    gainDb: Number.isFinite(Number(opt.gainDb)) ? Number(opt.gainDb) : OUT.gainDb,
  };
  const tag = opt.tag || items[0].input.slice(0, 80);

  const st = state();
  if (!st.ok) {
    res.writeHead(503, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('服务端没有可用的 ffmpeg，无法转成 AlphaPi 格式：' + (st.err || '未知原因')
      + '\n修复：容器镜像已内置 ffmpeg（Dockerfile 里 apk add ffmpeg），'
      + '本地 node 直跑请先装 ffmpeg 或设置 FFMPEG_PATH。');
    return Promise.resolve('no-ffmpeg');
  }

  const headers = {
    'Content-Type': raw ? 'application/octet-stream' : 'audio/wav',
    'Cache-Control': 'no-cache',
    // ★ 转码流的字节偏移和源文件无关 ⇒ 明确告诉客户端「不支持 Range」，
    //   免得它按 Range 探一眼再按字节去猜时长（R1 的播放器会这么干）。
    'Accept-Ranges': 'none',
    'X-Audio-Format': (raw ? 'pcm' : 'wav') + '/pcm_u' + o.bits + '/' + o.rate + '/' + o.ch,
    'X-Transcoded-From': items.length > 1 ? 'mix' : items[0].kind,
    'X-Track-Count': String(items.length),
  };

  if (req.method === 'HEAD') {
    res.writeHead(200, headers);
    res.end();
    return Promise.resolve('done');
  }

  let child;
  try {
    child = spawn(FFMPEG, buildArgs(items, o), { stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (e) {
    res.writeHead(502, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('启动 ffmpeg 失败：' + String(e && e.message || e));
    return Promise.resolve('failed');
  }

  const header = raw ? null : wavHeader(o.rate, o.bits, o.ch);
  let started = false;
  let bytes = 0;
  let finished = false;
  let errTail = '';
  let startTimer = null;
  let idleTimer = null;

  /**
   * ★ v1.6.0：把「这条流到底送出去多少秒」交给调用方结算播放记录。
   *   bytes 是**写进 socket** 的字节数，比真正听过的位置略微超前（TCP 发送缓冲 +
   *   设备 20KB FIFO ≈ 不超过十几秒），所以续播那边统一再回退 10s 兜住。
   *   只报一次：正常播完（child close）/ 客户端掐断（res close）/ 出错被 kill 都走它。
   */
  const bps = Math.max(1, o.rate * (o.bits / 8) * o.ch);
  // ---- v1.6.6 paced send ----
  const tickSlice = Math.max(1, Math.round(bps * PACE_TICK_MS / 1000));   // 24k → 480B
  const prebufBytes = Math.round(bps * PACE_PREBUF_MS / 1000);
  const maxQBytes = Math.round(bps * PACE_MAXQ_MS / 1000);
  let paceQ = [], paceQBytes = 0, paceTimer = null, paceOpen = false, paceLastAt = 0;
  let paceEof = false, resBlocked = false, pendingEndReason = null, downStallAt = 0;
  const stopPace = () => {
    if (paceTimer) { clearInterval(paceTimer); paceTimer = null; }
    paceQ.length = 0;
    paceQBytes = 0;
  };
  const paceFinish = () => {
    stopPace();
    if (pendingEndReason) { reportEnd(pendingEndReason); pendingEndReason = null; }
    try { res.end(); } catch { /* ignore */ }
  };
  const paceLoop = () => {
    const now = Date.now();
    if (!paceOpen && paceQBytes < prebufBytes) { paceLastAt = now; return; }  // 起播预缓冲
    paceOpen = true;
    // ★ 按**墙钟经过时间**算本拍应发字节，不能每拍固定 tickSlice：
    //   setInterval(20ms) 在 Windows 上会被量化到 ~31ms/拍，固定 480B/拍就只有
    //   15.4KB/s < 设备消耗的 24KB/s ⇒ 越播越欠供。补量后平均速率恒等于 bps；
    //   单拍上限 250ms 防止进程被挂起后一次性把 2s 存货全砸给设备（会撑爆 FIFO）。
    const elapsed = Math.min(Math.max(now - (paceLastAt || now), 0), 250);
    paceLastAt = now;
    if (resBlocked) {
      // ★★ v1.6.7：下游不读 ≠ 上游断流。旧逻辑在这里直接 return，既不再 bumpIdle
      //   也不 resume，上游 ffmpeg 又被队列背压 pause 住 ⇒ 20s 空闲看门狗按
      //   「上游没数据」把流杀掉，只留一句误导性的「转码中断 code=null 已发 480B」。
      //   现在续着上游看门狗，另用独立的下游超时兜底（默认 60s）。
      bumpIdle();
      if (!downStallAt) downStallAt = now;
      if (now - downStallAt >= DOWN_TIMEOUT_MS) kill('下游 ' + DOWN_TIMEOUT_MS + 'ms 未读');
      return;
    }
    downStallAt = 0;
    let n = Math.max(tickSlice, Math.round(bps * elapsed / 1000));
    while (n > 0 && paceQ.length) {
      const c = paceQ[0];
      let part;
      if (c.length <= n) { part = c; paceQ.shift(); }
      else { part = c.slice(0, n); paceQ[0] = c.slice(n); }
      n -= part.length;
      paceQBytes -= part.length;
      bytes += part.length;                              // onEnd 只算真正写出去的
      if (!res.write(part)) { resBlocked = true; break; }
    }
    if (!resBlocked) {
      bumpIdle();                                        // 还在发 ⇒ 不算上游空闲
      if (paceQBytes < maxQBytes / 2) { try { child.stdout.resume(); } catch { /* ignore */ } }
    }
    if (paceQBytes === 0 && paceEof) paceFinish();
  };
  const startPace = () => {
    if (!PACE_ON || paceTimer || finished) return;
    paceTimer = setInterval(paceLoop, PACE_TICK_MS);
    if (paceTimer.unref) paceTimer.unref();
  };
  let reported = false;
  const reportEnd = reason => {
    if (reported) return;
    reported = true;
    if (typeof opt.onEnd !== 'function') return;
    try {
      opt.onEnd({ reason, bytes, sec: bytes / bps, started: started });
    } catch { /* 记账失败绝不能影响播放 */ }
  };

  const cleanup = () => {
    if (startTimer) { clearTimeout(startTimer); startTimer = null; }
    if (idleTimer) { clearTimeout(idleTimer); idleTimer = null; }
    try { child.stdout.removeAllListeners('data'); } catch { /* ignore */ }
  };
  const kill = why => {
    if (finished) return;
    finished = true;      // ★v1.6.7：不置的话 child close 会二次收场，打出误导性的
                          //   「转码中断 code=null」并在已 destroy 的 res 上再调 end()
    cleanup();
    // 日志必须在 stopPace() 之前打 —— 它会把队列清零，队列长度是定责的关键证据
    console.log('[ap] 转码中止 ' + tag + ' reason=' + why + ' 已发 ' + bytes + 'B'
      + (PACE_ON ? ' / 队列 ' + paceQBytes + 'B / 下游阻塞=' + resBlocked : ''));
    stopPace();
    reportEnd('kill:' + why);
    try { child.kill('SIGKILL'); } catch { /* already dead */ }
    if (!started) {
      // 还没发头 ⇒ 还能给一句人话（把 ffmpeg 的英文报错翻成中文）
      try {
        res.writeHead(502, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('转码失败：' + explainErr(errTail || why));
      } catch { /* client gone */ }
    } else {
      // 已经发了 200：只能断连接（res.end() 会被当成「正常播完」）
      try { res.destroy(); } catch { /* ignore */ }
    }
  };

  const bumpIdle = () => {
    if (idleTimer) clearTimeout(idleTimer);
    idleTimer = setTimeout(() => kill('空闲 ' + IDLE_TIMEOUT + 'ms 无数据'), IDLE_TIMEOUT);
    if (idleTimer.unref) idleTimer.unref();
  };

  startTimer = setTimeout(() => kill('首字节超时 ' + START_TIMEOUT + 'ms'), START_TIMEOUT);
  if (startTimer.unref) startTimer.unref();

  child.on('error', e => {
    errTail = String(e && e.message || e);
    kill(errTail);
  });
  child.stderr.on('data', d => {
    errTail = (errTail + d.toString('utf8')).slice(-400);
  });

  child.stdout.on('data', chunk => {
    if (finished) return;
    if (!started) {
      started = true;
      if (startTimer) { clearTimeout(startTimer); startTimer = null; }
      try {
        res.writeHead(200, headers);
        if (header) res.write(header);
      } catch (e) {
        errTail = String(e && e.message || e);
        return kill('写响应头失败');
      }
    }
    bumpIdle();
    if (PACE_ON) {
      // ★ v1.6.6：先入队，由 paceLoop 匀速吐出；队列顶到 maxQBytes 就 pause ffmpeg，
      //   服务端始终握着 1~2s 存货（本地文件 ffmpeg 全速产出，几毫秒就能攒满）。
      paceQ.push(chunk);
      paceQBytes += chunk.length;
      startPace();
      if (paceQBytes >= maxQBytes) child.stdout.pause();
    } else {
      // 旧逻辑：Node 的 res.write() 永远「成功」，返回 false 只是「先别塞了」——
      //   靠 pause/resume 把背压传回 ffmpeg，绝不丢数据（radioproxy 里踩过）。
      bytes += chunk.length;
      if (!res.write(chunk)) child.stdout.pause();
    }
  });

  res.on('drain', () => {
    resBlocked = false;
    if (!PACE_ON) { try { child.stdout.resume(); } catch { /* ignore */ } }
  });
  const done = () => {
    if (finished) return;
    finished = true;
    cleanup();
    stopPace();
    // ★ v1.6.0：客户端主动结束（设备说停 / 切歌 / 关机）—— 播放位置就到这里
    reportEnd('client-close');
    try { child.kill('SIGKILL'); } catch { /* ignore */ }
  };
  // 客户端掐连接（切歌 / 拖进度 / 关设备）是常态，不是错误
  res.on('close', done);
  res.on('error', done);

  child.on('close', code => {
    if (finished) return;
    finished = true;
    cleanup();
    // ★ v1.6.6：paced 队列里还有音频没发完 ⇒ 不能立刻 end/reportEnd，
    //   否则尾巴被切掉、onEnd 的字节数也少算。交给 paceLoop 排空后 paceFinish 收尾。
    if (PACE_ON && started && paceQBytes > 0 && paceTimer) {
      paceEof = true;
      pendingEndReason = 'eof:' + code;
      return;
    }
    stopPace();
    reportEnd('eof:' + code);
    if (!started) {
      const msg = explainErr(errTail);
      console.log('[ap] 转码失败 ' + tag + ' code=' + code + ' — '
        + msg + (errTail ? ' ｜ ffmpeg: ' + errTail.replace(/\s+/g, ' ').slice(0, 160) : ''));
      try {
        res.writeHead(502, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('转码失败：' + msg);
      } catch { /* client gone */ }
      return;
    }
    try { res.end(); } catch { /* ignore */ }
    if (code !== 0 && code !== 255) {
      console.log('[ap] 转码中断 ' + tag + ' code=' + code + ' 已发 ' + bytes + 'B '
        + (errTail ? errTail.replace(/\s+/g, ' ').slice(0, 160) : ''));
    }
  });

  return new Promise(resolve => {
    const fin = r => resolve(r);
    res.on('close', () => fin('done'));
    // ★ v1.6.6：paced 队列还没排空时，ffmpeg 退出不代表这条流结束 ——
    //   提前 resolve 会让调用方以为播完了（实测 72000B 只发了 24480B 就返回）。
    //   此时由 paceFinish → res.end() → res close 来兑现 Promise。
    child.on('close', () => { if (!paceEof) fin('done'); });
  });
}

// ---------------------------------------------------------------- 自检

/**
 * 转码自检：不依赖任何外部源，用 ffmpeg 自带的正弦波走一遍完整链路，
 * 报告「跑没跑起来 / 出了多少字节 / 字节率对不对」。
 * 用途：NAS 上第 101 次「点了没声音」时，先分清是转码层还是取链层的问题。
 */
function selftest(seconds) {
  const sec = clampInt(seconds, 1.5, 0.5, 10);
  return new Promise(resolve => {
    const st = state();
    if (!st.ok) return resolve({ ok: false, err: '没有可用的 ffmpeg：' + (st.err || ''), ver: '' });
    const t0 = Date.now();
    let bytes = 0;
    let err = '';
    const child = spawn(FFMPEG, [
      '-hide_banner', '-nostdin', '-loglevel', 'error',
      '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=' + sec,
      '-ac', String(OUT.ch), '-ar', String(OUT.rate),
      '-acodec', OUT.bits === 16 ? 'pcm_s16le' : 'pcm_u8',
      '-f', OUT.bits === 16 ? 's16le' : 'u8', 'pipe:1',
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    child.stdout.on('data', d => { bytes += d.length; });
    child.stderr.on('data', d => { err += d.toString('utf8'); });
    child.on('error', e => resolve({ ok: false, err: String(e && e.message || e), ver: st.ver }));
    child.on('close', code => {
      const expect = Math.round(OUT.rate * OUT.ch * (OUT.bits / 8) * sec);
      resolve({
        ok: code === 0 && bytes > 0,
        ver: st.ver,
        code,
        bytes,
        expect,
        ratio: expect ? +(bytes / expect).toFixed(3) : 0,
        ms: Date.now() - t0,
        format: 'pcm_u' + OUT.bits + '/' + OUT.rate + '/' + OUT.ch,
        err: err.replace(/\s+/g, ' ').slice(0, 200),
      });
    });
  });
}

/** 给前端 / health 用的只读快照（不含 err 全文，避免页面被吓到） */
function info() {
  const st = state();
  return {
    ok: !!st.ok,
    ver: st.ver || '',
    err: st.ok ? '' : String(st.err || '').slice(0, 160),
    format: 'pcm_u' + OUT.bits + '/' + OUT.rate + '/' + OUT.ch,
    rate: OUT.rate, bits: OUT.bits, ch: OUT.ch,
    gainDb: OUT.gainDb || 0,
    startTimeoutMs: START_TIMEOUT,
    idleTimeoutMs: IDLE_TIMEOUT,
    pace: PACE_ON, paceTickMs: PACE_TICK_MS, prebufMs: PACE_PREBUF_MS,
    maxQueueMs: PACE_MAXQ_MS, downTimeoutMs: DOWN_TIMEOUT_MS,
  };
}

module.exports = { check, state, serve, selftest, info, wavHeader, OUT, _buildArgs: buildArgs };
