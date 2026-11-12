// 小智曲库 play() 被打断行为的真机（无头 Edge）回归测试
// A 连点两首：全程不该出现「播放失败」（旧版会闪一下）
// B 坏链：必须仍然报「播放失败」（确认没把真失败一起吞掉）
// C 单点正常文件：正常播放
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const ROOT = process.argv[2] || path.join(__dirname, '..', 'web', 'index.html');
const EDGE = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';

function wav(ms) {
  const rate = 8000, n = Math.round(rate * ms / 1000);
  const b = Buffer.alloc(44 + n);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n, 4); b.write('WAVE', 8);
  b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20);
  b.writeUInt16LE(1, 22); b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate, 28);
  b.writeUInt16LE(1, 32); b.writeUInt16LE(8, 34); b.write('data', 36);
  b.writeUInt32LE(n, 40);
  for (let i = 0; i < n; i++) b[44 + i] = 128;
  return b;
}

const SRV = http.createServer((req, res) => {
  const u = req.url.split('?')[0];
  if (u === '/' || u === '/index.html') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    return res.end(fs.readFileSync(ROOT));
  }
  if (u === '/slow.wav' || u === '/slow2.wav') {
    return setTimeout(() => {
      const b = wav(1500);
      res.writeHead(200, { 'Content-Type': 'audio/wav', 'Content-Length': b.length });
      res.end(b);
    }, 900);
  }
  if (u === '/fast.wav') {
    const b = wav(1500);
    res.writeHead(200, { 'Content-Type': 'audio/wav', 'Content-Length': b.length });
    return res.end(b);
  }
  if (u.startsWith('/api/')) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end('{"ok":true,"flags":{},"books":[],"items":[]}');
  }
  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('nope');
});

function get(url) {
  return new Promise((ok, bad) => http.get(url, r => {
    let d = ''; r.on('data', c => d += c); r.on('end', () => ok(d));
  }).on('error', bad));
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  await new Promise(r => SRV.listen(0, '127.0.0.1', r));
  const port = SRV.address().port;
  const edge = spawn(EDGE, [
    '--headless=new', '--remote-debugging-port=9334',
    '--user-data-dir=C:/Temp/edge-audit-' + Date.now(),
    '--no-first-run', '--no-default-browser-check', '--disable-gpu',
    '--autoplay-policy=no-user-gesture-required', 'about:blank',
  ], { stdio: 'ignore' });

  let ws, id = 0;
  const pending = new Map();
  const send = (method, params) => new Promise((ok, bad) => {
    const i = ++id; pending.set(i, { ok, bad });
    ws.send(JSON.stringify({ id: i, method, params: params || {} }));
  });

  let target = null;
  for (let i = 0; i < 40 && !target; i++) {
    await sleep(250);
    try {
      const list = JSON.parse(await get('http://127.0.0.1:9334/json'));
      target = list.find(t => t.type === 'page');
    } catch { /* 还没起 */ }
  }
  if (!target) { console.log('❌ CDP 起不来'); edge.kill(); process.exit(1); }

  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, bad) => { ws.onopen = ok; ws.onerror = bad; });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const p = pending.get(m.id); pending.delete(m.id);
      m.error ? p.bad(new Error(m.error.message)) : p.ok(m.result);
    }
  };

  await send('Page.enable');
  await send('Runtime.enable');
  await send('Page.navigate', { url: 'http://127.0.0.1:' + port + '/' });
  await sleep(1200);

  const evaluate = async (expr, gesture) => {
    const r = await send('Runtime.evaluate', {
      expression: expr, awaitPromise: true, returnByValue: true, userGesture: gesture !== false,
    });
    return r.result && r.result.value;
  };

  // 记录 msg 的每一次变化：「播放失败」常常只在切歌瞬间闪一下，
  // 之后成功的 play() 会用 msg('') 把它清掉，只看最终值必然漏判。
  await evaluate("(function(){ if(window.__msgHooked) return 'already';"
    + " var f=window.msg; window.__msgHist=[];"
    + " window.msg=function(s,c){ window.__msgHist.push(String(s||'')); return f(s,c); };"
    + " window.__msgHooked=1; return 'hooked'; })()");

  const reset = () => evaluate("(document.getElementById('msg').textContent='',window.__msgHist=[],"
    + "au.pause(),au.removeAttribute('src'),au.load(),'ok')");

  let pass = 0, fail = 0;
  const check = (name, cond, extra) => {
    console.log((cond ? '✅ ' : '❌ ') + name + (extra ? '  → ' + extra : ''));
    cond ? pass++ : fail++;
  };

  // ---------- A：连点两首（第一首还在取链，第二首已换） ----------
  await reset();
  await evaluate("(play({url:'/slow.wav',title:'A'},'local'),1)");
  await sleep(80);
  await evaluate("(play({url:'/fast.wav',title:'B'},'local'),1)");
  await sleep(2200);
  const histA = await evaluate("JSON.stringify(window.__msgHist)");
  const npA = await evaluate("document.getElementById('npA').textContent");
  const badA = /播放失败/.test(histA || '');
  check('A 连点两首：全程不出现「播放失败」', !badA,
    '历史=' + histA + (badA ? '  ← 闪了一下，其实第二首正在播' : '') + ' 当前曲目=' + npA);

  // ---------- C：单点正常文件 ----------
  await reset();
  await evaluate("(play({url:'/fast.wav',title:'C'},'local'),1)");
  await sleep(1200);
  const pausedC = await evaluate('au.paused');
  const histC = await evaluate("JSON.stringify(window.__msgHist)");
  check('C 单点正常文件：正在播且无报错', pausedC === false && !/播放失败/.test(histC || ''),
    'paused=' + pausedC + ' 历史=' + histC);

  // ---------- B：坏链，必须仍然报错 ----------
  await reset();
  await evaluate("(play({url:'/nope.wav',title:'D'},'local'),1)");
  await sleep(1500);
  const histB = await evaluate("JSON.stringify(window.__msgHist)");
  check('B 坏链：仍然报「播放失败」（没被静默吞掉）', /播放失败/.test(histB || ''), '历史=' + histB);

  console.log('\n结果：' + pass + ' 通过 / ' + fail + ' 失败');
  edge.kill();
  SRV.close();
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error('❌', e.message); process.exit(1); });
