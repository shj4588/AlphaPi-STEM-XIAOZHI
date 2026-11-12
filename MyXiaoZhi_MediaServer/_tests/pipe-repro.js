#!/usr/bin/env node
/**
 * pipeAudio 崩溃复现/回归测试
 *
 *   直接从 server.js 里抠出 pipeAudio 的函数体来跑（不是复制一份逻辑，改哪测哪），
 *   把里面的 20000ms 超时按比例缩到 1200ms 好让测试几秒内出结果。
 *
 * 场景：上游稳稳地吐数据 → 客户端读一点就「暂停」（模拟 R1 播放器暂停/拖进度，
 *       socket 缓冲填满 → pipe 背压 → 上游 body 没人消费）→ 再恢复读。
 *
 *   旧版（AbortSignal.timeout 一把梭）：超时定时器照跑 → abort → fromWeb 的流
 *   error 没人接 → unhandled 'error' → 进程退出（= 容器偶发停止）
 *   新版：头到了就 clearTimeout，传输期只留空闲看门狗 → 暂停多久都不炸
 *
 * 用法：node pipe-repro.js <server.js 路径>
 */
const fs = require('fs');
const http = require('http');
const path = require('path');

const serverFile = process.argv[2] || path.join(__dirname, '..', 'server.js');
const src = fs.readFileSync(serverFile, 'utf8');

const a = src.indexOf('async function pipeAudio');
const b = src.indexOf('const MIN_FULL_BYTES');
if (a < 0 || b < 0) { console.error('抠不到 pipeAudio，检查 server.js 是否改过结构'); process.exit(2); }

let fnText = src.slice(a, b);
// 两个超时常量定义在函数外面（改代码时就近放），抠函数体时补回来，再按比例缩短
if (!/CONNECT_MS\s*=/.test(fnText)) fnText = 'let CONNECT_MS = 20000;\nlet IDLE_MS = 60000;\n' + fnText;
// 把「等响应头」和「空闲看门狗」的时间都按比例缩短，测试才跑得动
fnText = fnText.replace(/CONNECT_MS\s*=\s*\d+/g, 'CONNECT_MS = 1200')
               .replace(/IDLE_MS\s*=\s*\d+/g, 'IDLE_MS = 1200')
               .replace(/AbortSignal\.timeout\(\s*20000\s*\)/g, 'AbortSignal.timeout(1200)');

const { Readable } = require('stream');
let pipeAudio;
try {
  pipeAudio = new Function('Readable', fnText + '\nreturn pipeAudio;')(Readable);
} catch (e) {
  console.error('函数体解析失败：' + e.message);
  process.exit(2);
}

// ---- 上游：每 80ms 吐 4KB，一共 400 块（约 1.6MB，够撑过暂停窗口）----
const TOTAL = 400;
const upstream = http.createServer((rq, rs) => {
  rs.writeHead(200, { 'Content-Type': 'audio/mpeg', 'Content-Length': String(TOTAL * 4096) });
  let n = 0;
  const t = setInterval(() => {
    if (n >= TOTAL) { clearInterval(t); try { rs.end(); } catch {} return; }
    n++;
    try { rs.write(Buffer.alloc(4096, 0x41)); } catch { clearInterval(t); }
  }, 80);
  rs.on('close', () => clearInterval(t));
});

upstream.listen(0, '127.0.0.1', () => {
  const upUrl = 'http://127.0.0.1:' + upstream.address().port + '/a.m4a';

  const front = http.createServer((req, res) => {
    res.on('error', () => {});                 // 前置端自己也要扛住 EPIPE
    pipeAudio(res, req, upUrl).catch(e => { console.log('[front] 调用抛错：' + e.message); });
  });

  front.listen(0, '127.0.0.1', () => {
    const port = front.address().port;
    const req = http.get({ host: '127.0.0.1', port, path: '/s/0' }, r => {
      let got = 0;
      r.on('data', c => { got += c.length; });
      r.on('error', () => {});
      // 先正常读 0.4s，然后「暂停」6s（远超 1200ms 超时窗口），再恢复
      setTimeout(() => {
        r.pause();
        console.log('[client] 已暂停读取（模拟 R1 播放器暂停），等 6s…');
        setTimeout(() => {
          const before = got;
          r.resume();
          setTimeout(() => {
            console.log('[client] 恢复后新增 ' + (got - before) + ' 字节，累计 ' + got);
            const ok = got > before;
            console.log(ok ? '✅ 进程存活且流继续' : '⚠️ 流没恢复（但进程没死）');
            cleanup(ok ? 0 : 1);
          }, 1500);
        }, 6000);
      }, 400);
    });
    req.on('error', e => console.log('[client] 请求失败：' + e.message));
  });

  let done = false;
  function cleanup(code) {
    if (done) return; done = true;
    try { front.close(); } catch {}
    try { upstream.close(); } catch {}
    setTimeout(() => process.exit(code), 100);
  }
});

// 没有这个兜底的话，我们测的就是「进程会不会崩」——崩了就真崩，正好当断言
process.on('uncaughtException', e => {
  console.error('❌ 进程级未捕获异常（这就是容器退出的原因）：' + (e && e.message || e));
  process.exit(1);
});
