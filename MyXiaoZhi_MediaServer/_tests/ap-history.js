'use strict';
/**
 * 播放记录 / 设备识别 / 续播查表 的纯逻辑回归（不联网、不转码、不碰真实 files/）。
 *
 *   node _tests/ap-history.js
 *
 * 覆盖：
 *   ① devIdOf：MAC 头（带冒号/小写/非法字符）归一化；没头时退化 ip-<地址>
 *   ② histStart/histEnd：位置 = 续播起点 + 实际送出秒数；负数/NaN 归零
 *   ③ histResume：同一本书（关键词部分匹配）能找回来；换一本书不误命中；只认 book
 *   ④ 每台设备最多留 60 条；快照按最近活跃排序、列表裁 20 条
 *   ⑤ 落盘再重读，内容一致（原子写的 tmp+rename 路径也走一遍）
 */

const fs = require('fs');
const os = require('os');
const path = require('path');

const TMP = path.join(os.tmpdir(), 'ap-hist-test-' + process.pid + '.json');
process.env.HIST_FILE = TMP;
try { fs.unlinkSync(TMP); } catch { /* 本来就没有 */ }

let pass = 0, fail = 0;
const ok = (cond, msg, extra) => {
  if (cond) { pass++; console.log('  ✓ ' + msg); }
  else { fail++; console.log('  ✗ ' + msg + (extra !== undefined ? '  → ' + JSON.stringify(extra) : '')); }
};
const eq = (a, b, msg) => ok(JSON.stringify(a) === JSON.stringify(b), msg, { got: a, want: b });

const req = (mac, ip) => ({
  headers: mac ? { 'x-dev-mac': mac, 'x-dev-name': 'STEM-客厅' } : {},
  socket: { remoteAddress: ip || '192.168.2.104' },
  url: '/ap/play',
});

function fresh() {
  delete require.cache[require.resolve('../server.js')];
  return require('../server.js');
}

console.log('\n[1] 设备识别');
{
  const S = fresh();
  eq(S.devIdOf(req('f0:9e:9e:cb:6f:bc')), 'F09E9ECB6FBC', 'MAC 归一化成 12 位大写');
  eq(S.devIdOf(req('3c-84-27-89-73-a8')), '3C84278973A8', '连字符也能认');
  eq(S.devIdOf(req('')), 'ip-192.168.2.104', '没报 MAC → ip-<地址>');
  eq(S.devIdOf({ headers: {}, socket: { remoteAddress: '::ffff:10.0.0.9' } }), 'ip-10.0.0.9',
    'IPv4-mapped 地址去掉 ::ffff:');
  ok(S.devIdOf(req('abc')) === 'ip-192.168.2.104', 'MAC 太短（<8 位）不认，退回 IP');
}

console.log('\n[2] 起播 / 结算');
{
  const S = fresh();
  const r1 = req('F0:9E:9E:CB:6F:BC');
  const e = S.histStart(r1, {
    kind: 'book', q: '三体', book: '地球往事Ⅰ', ep: 12, off: 300, title: '第12集',
  });
  ok(e.id && e.ts > 0, '起播立刻有一条记录');
  eq(e.pos, 0, '刚起播 pos=0');
  S.histEnd(e, 61.4);
  eq(e.pos, 361.4, 'pos = 起点 300s + 实际 61.4s');
  eq(e.done, true, '标记 done');

  const e2 = S.histStart(r1, { kind: 'song', q: '周杰伦', title: '晴天' });
  S.histEnd(e2, -5);
  eq(e2.pos, 0, '负秒数归零，不会变成负进度');
  const e3 = S.histStart(r1, { kind: 'radio', q: '中国之声', title: 'CNR-1 中国之声' });
  S.histEnd(e3, undefined);
  eq(e3.pos, 0, 'undefined 秒数同样归零');

  const snap = S.histSnapshot();
  eq(snap.count, 1, '一台 MAC 只算一台设备');
  eq(snap.devices[0].name, 'STEM-客厅', '设备名取固件上报的 X-Dev-Name');
  eq(snap.devices[0].list[0].kind, 'radio', '最近的排在最前');
}

console.log('\n[3] 续播查表');
{
  const S = fresh();
  const r1 = req('F0:9E:9E:CB:6F:BC');
  const e = S.histStart(r1, { kind: 'book', q: '三体', book: '地球往事Ⅰ', ep: 12, off: 0 });
  S.histEnd(e, 734);
  const hit = S.histResume(r1, '三体');
  ok(!!hit && hit.ep === 12, '说「三体」能找回第 12 集', hit && hit.ep);
  ok(!!S.histResume(r1, '地球往事'), '按书名部分匹配也能命中');
  ok(S.histResume(r1, '鬼吹灯') === null, '换一本书不误命中');
  ok(S.histResume(req('3C:84:27:89:73:A8'), '三体') === null, '另一台设备看不到别人的进度');

  // 歌不算书：不能拿歌曲关键词去续播
  const s = S.histStart(r1, { kind: 'song', q: '三体主题曲', title: '三体主题曲' });
  S.histEnd(s, 100);
  ok(!!S.histResume(r1, '三体'), '只扫 book 条目，歌曲不干扰');
}

console.log('\n[4] 条数上限 / 快照裁剪');
{
  const S = fresh();
  const r1 = req('AAAA11112222');
  for (let i = 0; i < 70; i++) {
    const e = S.histStart(r1, { kind: 'song', q: 'q' + i, title: 't' + i });
    S.histEnd(e, i);
  }
  const snap = S.histSnapshot();
  eq(snap.devices[0].list.length, 20, '快照只给最近 20 条');
  eq(snap.devices[0].list[0].title, 't69', '最新的在最前');
  // 内存里保留 60 条
  const dev = S.histLoad().devices['AAAA11112222'];
  eq(dev.list.length, 60, '每台设备内存里最多 60 条');
}

console.log('\n[5] 落盘 / 重读');
{
  const S = fresh();
  const r1 = req('AAAA11112222');
  const e = S.histStart(r1, { kind: 'book', q: '三体', book: '地球往事Ⅰ', ep: 7, off: 120 });
  S.histEnd(e, 240);
  S.histFlush();
  ok(fs.existsSync(TMP), '写盘生成了 history.json');
  ok(!fs.existsSync(TMP + '.tmp'), '没有残留 .tmp（rename 成功）');
  const onDisk = JSON.parse(fs.readFileSync(TMP, 'utf8'));
  ok(onDisk.ver === 1 && Object.keys(onDisk.devices).length === 1, '落盘内容结构正确', onDisk);
  eq(onDisk.devices.AAAA11112222.last.pos, 360, '落盘的播放位置 = 120 + 240');

  const S2 = fresh();           // 重新加载模块 = 重新读盘
  const snap2 = S2.histSnapshot();
  ok(snap2.count === 1 && snap2.devices[0].list[0].ep === 7, '重启后记录还在', snap2);
  ok(S2.histResume(r1, '三体').pos === 360, '重启后续播点还是 360s');
}

console.log('\n[6] 自动续播：这集真播完 → 下次从下一集开头');
{
  const S = fresh();
  const r1 = req('BBBB33334444');
  // ① 中途断连（没有 reason）绝不能当成"播完"，否则整本书被跳过一集
  const e = S.histStart(r1, { kind: 'book', q: '凡人修仙传', book: '凡人修仙传', ep: 5 });
  S.histEnd(e, 678);
  ok(!e.fin, '没有 reason 的结算不算"播完"');
  const rm1 = S.histResume(r1, '凡人修仙传');
  eq([rm1.ep, rm1.pos], [5, 678], '中途断连 ⇒ 还是第 5 集、位置保留');

  // ② ffmpeg 自己走完 = 这一集听完
  const e2 = S.histStart(r1, { kind: 'book', q: '凡人修仙传', book: '凡人修仙传', ep: 5 });
  S.histEnd(e2, 690, { reason: 'eof:0' });
  ok(e2.fin === true, 'ffmpeg 自己走完 ⇒ fin');
  const rm2 = S.histResume(r1, '凡人修仙传');
  eq([rm2.ep, rm2.pos, rm2.off, rm2.nextEp], [6, 0, 0, true], '下次续播 = 第 6 集开头（不再回放末尾 10s）');

  // ③ 设备主动断开（切歌/说停）也不算播完
  const e3 = S.histStart(r1, { kind: 'book', q: '凡人修仙传', book: '凡人修仙传', ep: 6 });
  S.histEnd(e3, 400, { reason: 'client-close' });
  ok(!e3.fin, '设备自己断开（client-close）不算播完');
  eq(S.histResume(r1, '凡人修仙传').ep, 6, 'client-close ⇒ 仍在第 6 集');
}

console.log('\n[7] v1.6.3 设备上报的位置才是权威（字节数在本地文件上完全失真）');
{
  const S = fresh();
  const r = req('CCCC55556666');

  // ① 设备这条流：onEnd 的字节数不许写 pos（实测 ss=600 只读 5 秒，字节却等于整集 77 秒）
  const e = S.histStart(r, {
    kind: 'book', q: '凡人修仙传', book: '凡人修仙传', ep: 3, off: 600, pos: 600, dur: 900,
  });
  S.histEnd(e, 77.6, { reason: 'eof:0', trustBytes: false });
  eq(e.pos, 600, 'AlphaPi 流：onEnd 不动 pos（起播点先顶上）');
  ok(!e.fin, 'AlphaPi 流：onEnd 不许判"播完"（只有设备能说）');

  // ② 设备上报绝对位置：625 就是 625，不能再加起播点 600
  e.pos = 600;
  const abs = 625;
  e.pos = Math.min(abs, e.dur);       // 等价于 /ap/pos 的处理
  e.devsync = true;
  eq(e.pos, 625, '设备上报是"集内绝对位置"，不叠加起播点');

  // ③ 设备说了"本集播完" ⇒ 下次续播下一集；随后 onEnd 再来也不许改
  e.fin = true; e.paused = true;
  S.histEnd(e, 2400, { reason: 'eof:0' });
  eq([e.pos, e.fin], [625, true], 'devsync 之后 onEnd 完全无效');
  eq([S.histResume(r, '凡人修仙传').ep, S.histResume(r, '凡人修仙传').nextEp], [4, true],
    '设备报 fin ⇒ 下次续播第 4 集开头');

  // ④ 旧版污染数据（pos 贴到集尾甚至超出时长）⇒ 当成本集听完，不许 -ss 到文件外
  const S2 = fresh();
  const r2 = req('DDDD77778888');
  const bad = S2.histStart(r2, {
    kind: 'book', q: '凡人修仙传', book: '凡人修仙传', ep: 2, off: 0, pos: 999, dur: 899,
  });
  bad.pos = 999; bad.done = true;
  const rm = S2.histResume(r2, '凡人修仙传');
  eq([rm.ep, rm.nextEp, rm.off], [3, true, 0], 'pos ≥ 时长-3 ⇒ 视为听完，续下一集');

  // ⑤ 没有任何历史 ⇒ null（调用方退回本地库 lastEp）
  eq(S2.histResume(r2, '不存在的书'), null, '查不到就返回 null');
}

console.log('\n[8] v1.6.4 断电兜底：设备一条上报都没发出来时不许整段丢掉');
{
  const S = fresh();
  const r = req('EEEE99990000');

  // ① 听 40 分钟后被拔电：socket 不再消费 → ffmpeg 被背压停住 → 空闲看门狗 20s 后杀。
  //    字节数早就等于整集（ffmpeg 转本地文件远快于实时），必须被墙钟钳住。
  const e = S.histStart(r, {
    kind: 'book', q: '凡人修仙传', book: '凡人修仙传', ep: 7, off: 0, pos: 0, dur: 3000,
  });
  S.histEnd(e, 3000, { reason: '空闲 20000ms 无数据', trustBytes: false, wall: 2421 });
  eq([Math.round(e.pos), e.est], [2400, 'wall'], '断电：按 wall−20s 估位置，不用失真字节数');
  ok(!e.fin, '断电兜底不许判"播完"（下次还要继续这一集）');
  const rm = S.histResume(r, '凡人修仙传');
  eq([rm.ep, Math.round(rm.pos)], [7, 2400], '续播仍在这一集，位置≈断电前');

  // ② 设备发过心跳（/ap/pos?beat=1）⇒ 心跳值才是权威，墙钟兜底必须让位
  const S2 = fresh();
  const r2 = req('FFFF11112222');
  const e2 = S2.histStart(r2, {
    kind: 'book', q: '三体', book: '三体', ep: 2, off: 0, pos: 0, dur: 3000,
  });
  e2.pos = 1800; e2.devsync = true;     // ← 最后一次 30s 心跳写到这里
  S2.histEnd(e2, 3000, { reason: '空闲 20000ms 无数据', trustBytes: false, wall: 3000 });
  eq(e2.pos, 1800, '有心跳时以心跳为准，墙钟兜底不许覆盖');

  // ③ wall 比字节数还小（几秒转完整集、连接随后就断）⇒ 取较小值，别记成整集
  const S3 = fresh();
  const r3 = req('AAAA33334444');
  const e3 = S3.histStart(r3, {
    kind: 'book', q: '明朝那些事儿', book: '明朝那些事儿', ep: 1, off: 0, pos: 0, dur: 3000,
  });
  S3.histEnd(e3, 3000, { reason: 'eof:0', trustBytes: false, wall: 6 });
  eq(Math.round(e3.pos), 5, 'wall 只有 6s ⇒ 位置按 5s 记，不按整集');
}

try { fs.unlinkSync(TMP); } catch { /* ignore */ }

console.log('\n[9] 位置是否到集尾（apFinSane；v1.6.6 起只用于决定是否校正 pos）');
{
  const S = fresh();
  ok(S.apFinSane(true, 824, 828) === true, 'pos 离集尾 4s ⇒ 到尾');
  ok(S.apFinSane(true, 828, 828) === true, '正好在集尾 ⇒ 到尾');
  ok(S.apFinSane(true, 410, 828) === false, '410/828 ⇒ 没到尾');
  ok(S.apFinSane(true, 0, 0) === true, '不知道时长 ⇒ 不校正');
  ok(S.apFinSane(false, 500, 828) === false, 'fin=0 不适用');
}

console.log('\n[10] fin 不再被 pos 拦（v1.6.6：真播完一定能跳下一集）');
{
  const S = fresh();
  const e = { ep: 6, dur: 828, pos: 602, fin: false, done: false, paused: false };
  const r = S.apApplyPos(e, 602, true);
  ok(r.fin === true && r.corrected === true, 'pos=602/828 偏小 ⇒ 校正但不否定 fin');
  ok(e.fin === true && e.done === true && e.paused === true, 'fin/done/paused 全部落库');
  eq(e.pos, 828, '位置校正到集尾 828s');
  const e2 = { ep: 7, dur: 828, pos: 824, fin: false };
  const r2 = S.apApplyPos(e2, 824, true);
  ok(r2.corrected === false && e2.fin === true, 'pos 已到尾 ⇒ 不校正，fin 照记');
  eq(e2.pos, 824, '原位置保持');
  const e3 = { ep: 8, dur: 0, pos: 100, fin: false };
  S.apApplyPos(e3, 100, true);
  ok(e3.fin === true, '在线书 dur 未知 ⇒ 仍信设备 fin');
  eq(e3.pos, 100, 'dur 未知时不动位置');
  const e4 = { ep: 9, dur: 828, pos: 300, fin: false };
  S.apApplyPos(e4, 300, false);
  ok(e4.fin === false && e4.done === undefined, 'fin=0 ⇒ 不写任何收场字段');
}

console.log('\n[11] 集尾起点夹值（v1.6.8：dur-3，不是 dur-30）');
{
  const S = fresh();
  eq(S.apSeekClamp(769, 771), 768, '769/771 ⇒ 只倒回 1s（旧逻辑会夹到 741）');
  eq(S.apSeekClamp(741, 771), 741, '集尾前的正常起点不动');
  eq(S.apSeekClamp(900, 771), 768, '远超集尾 ⇒ 768，仍留 3s 音频不会零字节');
  eq(S.apSeekClamp(720, 771), 720, '用户「跳到第十二分钟」不受影响');
  eq(S.apSeekClamp(10, 2), 0, '极短集 ⇒ 0，不产生负偏移');
  eq(S.apSeekClamp(-5, 771), 0, '负数 ⇒ 0');
  eq(S.apSeekClamp(769, 0), 769, 'dur 未知（在线书）⇒ 不夹');
}

console.log('\n=== ' + (fail ? '有 ' + fail + ' 项失败' : '全部通过') + '（' + pass + ' 项）===\n');
process.exit(fail ? 1 : 0);
