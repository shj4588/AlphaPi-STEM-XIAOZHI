'use strict';
/**
 * loopcheck.js —— 验证「设备重连后会不会再听一遍开场」
 *
 * 这正是用户遇到的症状（同一句 6 秒内容无限循环）的复现脚本：
 *   序列模仿 NuCachedSource2 的行为 —— 连上听一会儿 → 断开 → 再连。
 *
 * 判据：
 *   · 第 1 次和第 2 次的**开头字节相同**  ⇒ 源被重新连接了，又把同一段开场吐了一遍 ⇒ 会循环 ✗
 *   · 两次开头**不同**                    ⇒ 上游连接没重建，听到的是连续的实时流 ⇒ 修好了 ✓
 */
const http = require('http');
const rp = require('./radioproxy');

const PORT = 8399;
const SELF = 'http://127.0.0.1:' + PORT;
// 用本地这台 PC 还能连的源（NAS 出口 IP 之前被同一个源限流过）
const SRC = process.argv[2] || 'http://lhttp.qtfm.cn/live/339/64k.mp3';
const GRAB_SEC = 8;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function grabOnce(sec) {
  return new Promise((resolve) => {
    const url = SELF + '/radio/' + rp.encTarget(SRC);
    const req = http.get(url, (res) => {
      // ★ 只留前 16KB 用于比对，别把整个流都攒进数组（攒了也没用，
      //   而且 8 秒能攒出几个 G，把判定输出淹了）。
      const head = [];
      let hn = 0, n = 0;
      res.on('data', (c) => {
        n += c.length;
        if (hn < 16384) {
          const need = 16384 - hn;
          const part = c.length > need ? c.slice(0, need) : c;
          head.push(part); hn += part.length;
        }
      });
      setTimeout(() => {
        req.destroy();
        resolve({ status: res.statusCode, total: n, buf: Buffer.concat(head) });
      }, sec * 1000);
    });
    req.on('error', (e) => resolve({ status: 'ERR ' + e.message, total: 0, buf: Buffer.alloc(0) }));
  });
}

(async () => {
  const srv = http.createServer((req, res) => {
    rp.serve(rp.encTarget(SRC), req, res, SELF);
  });
  await new Promise((r) => srv.listen(PORT, r));
  console.log('本地代理已起 ' + SELF);
  console.log('源: ' + SRC + '\n');

  console.log('① 第 1 次连接（听 ' + GRAB_SEC + ' 秒）…');
  const a = await grabOnce(GRAB_SEC);
  console.log('   HTTP=%s  收到 %d B', a.status, a.total);

  console.log('② 断开，等 2 秒（模拟设备 Cache full 后重连）…');
  await sleep(2000);
  const st = rp.stats();
  const k = Object.keys(st)[0];
  console.log('   此刻上游: subs=%s live=%s retryN=%s ringLen=%s',
    st[k] && st[k].subs, st[k] && st[k].live, st[k] && st[k].retryN, st[k] && st[k].ringLen);

  console.log('③ 第 2 次连接（同样 ' + GRAB_SEC + ' 秒）…');
  const b = await grabOnce(GRAB_SEC);
  console.log('   HTTP=%s  收到 %d B', b.status, b.total);

  console.log('\n──────── 判定 ────────');
  const N = Math.min(16384, a.total, b.total);
  if (N < 4096) {
    console.log('  ⚠ 数据量太小（%d B），源可能不可用，判定无意义', N);
  } else {
    const ha = a.buf.slice(0, N);
    const hb = b.buf.slice(0, N);
    const same = ha.equals(hb);
    console.log('  比对前 %d B：', N);
    console.log('    第1次 md5 = %s', require('crypto').createHash('md5').update(ha).digest('hex'));
    console.log('    第2次 md5 = %s', require('crypto').createHash('md5').update(hb).digest('hex'));
    console.log('  ⇒ %s', same
      ? '✗ 两次开头完全相同 —— 源被重新连接了，还是会听一遍开场（循环）'
      : '✓ 两次开头不同 —— 上游连接没重建，是连续的实时流');
  }

  const st2 = rp.stats();
  console.log('\n  上游统计:', JSON.stringify(st2).slice(0, 400));
  srv.close();
  setTimeout(() => process.exit(0), 300);
})();
