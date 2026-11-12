'use strict';
/**
 * _tests/ap-transcode.js —— AlphaPi 转码层回归测试（v1.4.0）
 *
 * 跑法（不需要 Docker，只要有 ffmpeg）：
 *   node _tests/ap-transcode.js
 *   FFMPEG_PATH=/usr/bin/ffmpeg node _tests/ap-transcode.js
 *
 * 覆盖：
 *   ① mp3 / flac → WAV 8bit/24000/单声道，头 44 字节逐字段 + 字节率 ≈ 24KB/s
 *   ② raw=1（裸 PCM，无头）
 *   ③ 增益参数
 *   ④ 非音频文件 → 502 + 中文原因（绝不能是「200 + 空响应」）
 *   ⑤ HEAD 不启动转码
 *   ⑥ 客户端中途断开后服务仍健康（ffmpeg 必须被 kill）
 *
 * 注意：本文件是**独立进程自测**，不参与 server.js 运行。
 */
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const FF = process.env.FFMPEG_PATH || 'ffmpeg';
process.env.FFMPEG_PATH = FF;
const tc = require('../transcode.js');

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'ap-tc-'));
const mp3 = path.join(TMP, 'a.mp3');
const flac = path.join(TMP, 'b.flac');
const bad = path.join(TMP, 'bad.txt');
fs.writeFileSync(bad, 'this is definitely not audio');

const gen = (out, extra) => execFileSync(FF, ['-y', '-hide_banner', '-loglevel', 'error',
  '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=3', ...extra, out]);
gen(mp3, ['-ac', '2', '-b:a', '128k']);
gen(flac, ['-ac', '2', '-c:a', 'flac']);

let failures = 0;
const ok = (cond, msg) => { console.log((cond ? '  PASS ' : '  FAIL ') + msg); if (!cond) failures++; };

const up = http.createServer((req, res) => {
  const f = req.url === '/a.mp3' ? mp3 : req.url === '/b.flac' ? flac : bad;
  if (!fs.existsSync(f)) { res.writeHead(404); return res.end('nope'); }
  res.writeHead(200, { 'Content-Type': 'audio/mpeg', 'Content-Length': fs.statSync(f).size });
  fs.createReadStream(f).pipe(res);
});

async function get(port, p, opts) {
  const r = await fetch('http://127.0.0.1:' + port + p, opts);
  return { status: r.status, headers: r.headers, body: Buffer.from(await r.arrayBuffer()) };
}

function checkWav(buf, label) {
  ok(buf.length > 44, label + ' 有数据 ' + buf.length + 'B');
  ok(buf.slice(0, 4).toString() === 'RIFF' && buf.slice(8, 12).toString() === 'WAVE', label + ' RIFF/WAVE');
  ok(buf.readUInt16LE(20) === 1, label + ' PCM=1');
  ok(buf.readUInt16LE(22) === 1, label + ' 单声道');
  ok(buf.readUInt32LE(24) === 24000, label + ' 采样率 24000');
  ok(buf.readUInt16LE(34) === 8, label + ' 位深 8');
  ok(buf.readUInt32LE(4) === 0xFFFFFFFF && buf.readUInt32LE(40) === 0xFFFFFFFF, label + ' 流式长度哨兵');
  const want = 24000 * 3;
  const got = buf.length - 44;
  ok(Math.abs(got - want) < want * 0.12, label + ' 字节数 ' + got + ' ≈ ' + want);
}

(async () => {
  await new Promise(r => up.listen(18099, '127.0.0.1', r));
  const probe = await tc.check();
  console.log('ffmpeg: ' + (probe.ok ? probe.ver : '不可用 — ' + probe.err));
  if (!probe.ok) { console.log('跳过（先装 ffmpeg 或设 FFMPEG_PATH）'); process.exit(2); }

  const down = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const map = { '/mp3': 'http://127.0.0.1:18099/a.mp3', '/flac': 'http://127.0.0.1:18099/b.flac' };
    const input = map[u.pathname];
    if (!input) return tc.serve(req, res, { input: bad, kind: 'file', tag: 'bad' });
    return tc.serve(req, res, {
      input, kind: 'url', tag: u.pathname,
      raw: u.searchParams.get('raw') === '1',
      gainDb: u.searchParams.get('gain') ? Number(u.searchParams.get('gain')) : undefined,
    });
  });
  await new Promise(r => down.listen(18098, '127.0.0.1', r));

  console.log('\n[1] mp3 → WAV');
  const a = await get(18098, '/mp3');
  ok(a.status === 200, 'HTTP ' + a.status);
  ok(/audio\/wav/.test(a.headers.get('content-type') || ''), 'Content-Type ' + a.headers.get('content-type'));
  ok(/none/.test(a.headers.get('accept-ranges') || ''), 'Accept-Ranges: none');
  checkWav(a.body, '[mp3]');

  console.log('\n[2] flac → WAV');
  const b = await get(18098, '/flac');
  ok(b.status === 200, 'HTTP ' + b.status);
  checkWav(b.body, '[flac]');

  console.log('\n[3] raw=1 裸 PCM');
  const raw = await get(18098, '/mp3?raw=1');
  ok(raw.status === 200 && /octet-stream/.test(raw.headers.get('content-type') || ''), 'application/octet-stream');
  ok(raw.body.slice(0, 4).toString() !== 'RIFF', '没有 WAV 头');
  ok(Math.abs(raw.body.length - 72000) < 8000, '字节数 ' + raw.body.length + ' ≈ 72000');

  console.log('\n[4] 增益');
  const g = await get(18098, '/mp3?gain=6');
  ok(g.status === 200, 'gain=6dB → HTTP ' + g.status);

  console.log('\n[5] 非音频 → 502 且有人话');
  const badRes = await get(18098, '/bad');
  ok(badRes.status === 502, 'HTTP ' + badRes.status);
  ok(/转码失败/.test(badRes.body.toString()), '中文原因：' + badRes.body.toString().replace(/\s+/g, ' ').slice(0, 60));

  console.log('\n[6] HEAD 不启动转码');
  const h = await get(18098, '/mp3', { method: 'HEAD' });
  ok(h.status === 200 && h.body.length === 0, '200 且无正文');

  console.log('\n[7] 客户端中途断开');
  await new Promise(resolve => {
    const rq = http.get('http://127.0.0.1:18098/mp3', r => {
      r.once('data', () => { rq.destroy(); setTimeout(resolve, 500); });
    });
    rq.on('error', () => resolve());
  });
  const ping = await new Promise(resolve => {
    http.get('http://127.0.0.1:18098/mp3', r => { r.destroy(); resolve(true); }).on('error', () => resolve(false));
  });
  ok(ping === true, '断开后仍能接受新请求');

  down.close(); up.close();
  try { fs.rmSync(TMP, { recursive: true, force: true }); } catch { /* ignore */ }
  console.log(failures ? '\n=== ' + failures + ' 项失败 ===' : '\n=== 全部通过 ===');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
