'use strict';
/**
 * `/ap/play`（v1.5.1）的**纯参数**回归：只验 ffmpeg 参数拼得对不对，不需要 ffmpeg。
 *
 * 为什么要单独一个：ap-transcode.js 要真跑 ffmpeg（本机常常没有），
 * 但"歌单拼流"最容易错的地方恰恰是参数（少一路 aresample、concat 写成 v=1、
 * 单输入还硬套 filter_complex），这些用数组断言就能盯住。
 *
 *   node _tests/ap-play-args.js
 */
const tc = require('../transcode');

let failures = 0;
const ok = (cond, msg) => {
  console.log((cond ? '  ✓ ' : '  ✗ ') + msg);
  if (!cond) failures++;
};

const O = { rate: 24000, bits: 8, ch: 1, gainDb: 0 };
const idxOf = (a, k) => a.indexOf(k);

console.log('[1] 单输入（单曲 / 电台）保持老行为');
{
  const a = tc._buildArgs([{ input: '/a.mp3', kind: 'file' }], O);
  ok(idxOf(a, '-map') > 0 && a[idxOf(a, '-map') + 1] === '0:a:0', '-map 0:a:0');
  ok(idxOf(a, '-vn') > 0, '保留 -vn');
  ok(idxOf(a, '-filter_complex') < 0, '没有 filter_complex');
  ok(a[a.length - 1] === 'pipe:1', '输出到 pipe:1');
  ok(a[idxOf(a, '-f') + 1] === 'u8', '8bit 裸流');
}

console.log('\n[2] 多输入 = 歌单连播');
{
  const items = [
    { input: '/a.mp3', kind: 'file' },
    { input: 'http://x/b.mp3', kind: 'url' },
    { input: '/c.flac', kind: 'file' },
  ];
  const a = tc._buildArgs(items, O);
  const fc = a[idxOf(a, '-filter_complex') + 1] || '';
  ok(idxOf(a, '-filter_complex') > 0, '有 filter_complex');
  ok(/concat=n=3:v=0:a=1/.test(fc), 'concat=n=3 且只拼音频');
  ok((fc.match(/aresample=24000/g) || []).length === 3, '每一路都 aresample（否则半速噪声）');
  ok((fc.match(/channel_layouts=mono/g) || []).length === 3, '每一路都统一声道');
  ok(fc.indexOf('[0:a]') >= 0 && fc.indexOf('[1:a]') >= 0 && fc.indexOf('[2:a]') >= 0, '三路都取 [i:a]');
  ok(idxOf(a, '-map') > 0 && a[idxOf(a, '-map') + 1] === '[out]', '-map [out]');
  ok(idxOf(a, '-vn') < 0, '多输入不需要 -vn（concat v=0）');
  ok((a.join(' ').match(/-i /g) || []).length === 3, '3 个 -i');
}

console.log('\n[3] 增益走 concat 之后，不和 -af 打架');
{
  const items = [{ input: '/a.mp3', kind: 'file' }, { input: '/b.mp3', kind: 'file' }];
  const a = tc._buildArgs(items, { ...O, gainDb: 3 });
  const fc = a[idxOf(a, '-filter_complex') + 1] || '';
  ok(idxOf(a, '-af') < 0, '没有 -af（-af 与 -filter_complex 互斥）');
  ok(/\[cat\]volume=3dB\[out\]/.test(fc), 'volume 挂在 concat 之后');
}

console.log('\n[4] URL 输入的容错参数只作用在下一路');
{
  const items = [{ input: '/a.mp3', kind: 'file' }, { input: 'http://x/b.mp3', kind: 'url' }];
  const a = tc._buildArgs(items, O);
  const i1 = idxOf(a, '-i');
  const i2 = a.indexOf('-i', i1 + 1);
  const rt = idxOf(a, '-rw_timeout');
  ok(rt > i1 && rt < i2, '-rw_timeout 夹在第一个 -i 与第二个 -i 之间');
  ok(idxOf(a, '-reconnect') > i1 && idxOf(a, '-reconnect') < i2, '-reconnect 同上');
}

console.log(failures ? '\n=== ' + failures + ' 项失败 ===' : '\n=== 全部通过 ===');
process.exit(failures ? 1 : 0);
