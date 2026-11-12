#!/usr/bin/env node
/**
 * lxrt.js —— 最小化「落雪」兼容运行时（headless LX）
 *
 * 目标：把落雪音乐（lyswhut/lx-music-mobile）里真正有价值的那一层剥出来，
 *      不依赖 React Native、不需要 UI，能在 Node 里直接跑。
 *
 * 依据（源码级，见 ../diag/lxmobile/lx-music-mobile）：
 *   · 落雪的「客户端」= musicSdk（纯 JS） + 音源脚本（JS 沙箱） + UI（RN）
 *   · 新版把内置取链实现全部删除（api-source.js 的 apiList 空、api-source-info.ts 空数组），
 *     取播放链接只剩「加载第三方音源脚本」一条路 ⇒ 沙箱是复刻的核心
 *   · 官方自定义源脚本约定 actions = ['musicUrl']（不含搜索）⇒ 搜索由内置 musicSdk 提供
 *
 * 本文件提供：
 *   1. LX 沙箱（globalThis.lx：request / on / send / utils / env / version / currentScriptInfo）
 *      —— 可加载任意 .js 音源脚本；明文脚本（如 ikun）可直接跑，
 *         带 obfuscator 完整性自检的脚本（如 sixyin）会拒绝在非真机环境运行。
 *   2. 内置搜索（移植 kw musicSearch 的真实参数，直连酷我公开接口，无需鉴权）
 *   3. HTTP 网关 /api/search，输出对齐 R1 侧契约 {total,list:[{index,title,artist,url}]}
 *
 * 用法：
 *   node lxrt.js info  <script.js>                       列出音源声明的 sources
 *   node lxrt.js url   <script.js> <source> <songId> [quality]
 *   node lxrt.js search <关键词> [条数]
 *   node lxrt.js play  <script.js> <关键词> [quality]    搜索 + 取链，输出直链
 *   node lxrt.js serve <script.js> --port 8099           起 HTTP 网关
 */
'use strict';

const fs = require('fs');
const vm = require('vm');
const path = require('path');
const nodeCrypto = require('crypto');

// 音源脚本常自发行事（如 ikun 加载时立刻 checkUpdate()），后端不可达会抛未处理的
// promise rejection。这类失败与我们的调用无关，不能让它把主进程带走。
process.on('unhandledRejection', () => {});

// ---------------------------------------------------------------- 内置搜索
// 移植自 src/utils/musicSdk/kw/musicSearch.js 的真实参数（本机实测通）
const KUWO_SEARCH = 'http://search.kuwo.cn/r.s' +
  '?client=kt&all={kw}&pn={pn}&rn={rn}&uid=794762570' +
  '&ver=kwplayer_ar_9.2.2.1&vipver=1&show_copyright_off=1&newver=1&ft=music' +
  '&cluster=0&strategy=2012&encoding=utf8&rformat=json&vermerge=1&mobi=1&issubtitle=1';
const UA_KW = 'Mozilla/5.0 (Linux; Android 9) kwplayer_ar_9.2.2.1';

async function kuwoSearch(keyword, limit = 20, page = 1) {
  const url = KUWO_SEARCH
    .replace('{kw}', encodeURIComponent(keyword))
    .replace('{pn}', String(page - 1))
    .replace('{rn}', String(limit));
  const r = await fetch(url, { headers: { 'User-Agent': UA_KW } });
  const d = await r.json();
  return (d.abslist || []).map(it => ({
    rid: String(it.MUSICRID || '').replace('MUSIC_', ''),
    title: it.NAME || '',
    artist: String(it.ARTIST || '').replace(/&/g, '/'),
    album: it.ALBUM || '',
  })).filter(x => x.rid);
}

// ---------------------------------------------------------------- lx.utils
// nativeModules/crypto.ts 是 RN 原生桥，这里用 Node crypto 平替同名接口
function makeUtils() {
  const real = {
    Buffer,
    String, Number, Array, Object, Math, Date, JSON,
    crypto: {
      md5: s => nodeCrypto.createHash('md5').update(String(s)).digest('hex'),
      hashSHA1: s => nodeCrypto.createHash('sha1').update(String(s)).digest('hex'),
      sha1: s => nodeCrypto.createHash('sha1').update(String(s)).digest('hex'),
      randomBytes: n => Buffer.from(nodeCrypto.randomBytes(n)).toString('hex'),
      aesEncryptSync: () => '',
      aesDecryptSync: () => '',
      rsaEncryptSync: () => '',
      rsaDecryptSync: () => '',
      aesEncrypt: async () => '',
      aesDecrypt: async () => '',
      rsaEncrypt: async () => '',
      rsaDecrypt: async () => '',
    },
    http: {},
  };
  // 未知属性兜底：避免混淆脚本因缺某个 utils 子项而中断（sixyin 仍会卡在自检，但至少不崩在这里）
  const magic = new Proxy(function () {}, {
    get(t, p) {
      if (p === 'from') return (s, e) => Buffer.from(String(s), e || 'utf8');
      if (p === 'toString') return () => '';
      if (p === Symbol.toPrimitive) return () => '';
      if (p === 'then') return undefined;
      return magic;
    },
    apply() { return magic; },
  });
  return new Proxy(real, { get: (t, p) => (p in t ? t[p] : magic) });
}

// ---------------------------------------------------------------- 沙箱 request
// 对齐 src/utils/request.js：默认 format='json'，resp.body = JSON.parse(...)
async function doRequest(url, options = {}) {
  const method = (options.method || options.type || 'GET').toUpperCase();
  const headers = Object.assign({
    'User-Agent': 'lx-music-mobile/1.9.1',
  }, options.headers || {});

  let body = options.body;
  if (options.form && !body) {
    body = Object.keys(options.form)
      .map(k => encodeURIComponent(k) + '=' + encodeURIComponent(options.form[k]))
      .join('&');
    headers['Content-Type'] = headers['Content-Type'] || 'application/x-www-form-urlencoded';
  } else if (options.formData && !body) {
    body = options.formData;
  } else if (body != null && typeof body === 'object') {
    body = JSON.stringify(body);
    headers['Content-Type'] = headers['Content-Type'] || 'application/json';
  }

  const timeout = options.timeout || 15000;
  const resp = await fetch(url, {
    method, headers, body,
    redirect: 'follow',
    signal: AbortSignal.timeout(timeout),
  });
  const text = await resp.text();
  const format = options.format || 'json';
  let parsed = text;
  if (format === 'json') {
    try { parsed = JSON.parse(text); } catch { parsed = text; }
  }
  return {
    statusCode: resp.status,
    headers: Object.fromEntries(resp.headers.entries()),
    body: parsed,
    raw: text,
  };
}

// ---------------------------------------------------------------- 脚本元信息
// 落雪音源脚本头部约定：块注释或行注释里的 @name / @description / @version / @author / @homepage
function parseMeta(code) {
  const head = String(code).slice(0, 4000);
  const meta = {};
  const re = /@(name|description|version|author|homepage)[:：]?\s*([^\n\r*]+)/gi;
  let m;
  while ((m = re.exec(head))) meta[m[1].toLowerCase()] = m[2].trim();
  return meta;
}

// ---------------------------------------------------------------- 运行时
class LXRuntime {
  constructor(opts = {}) {
    this.scripts = new Map();   // source -> { file, handler, qualitys }
    this.reqLog = [];
    this.meta = null;           // 脚本头部 @name/@version/... 元信息
    this.scriptName = null;
    this.opts = { verbose: opts.verbose !== false, env: opts.env || 'mobile', version: opts.version || '1.9.1' };
  }

  /** 加载一段音源代码，返回它声明的 sources（音源管理页的在线导入走这条） */
  loadCode(code, name = 'inline.js') {
    const self = this;
    const handlers = {};
    let inited = null;

    const lx = {
      EVENT_NAMES: { request: 'request', inited: 'inited', updateAlert: 'updateAlert' },
      env: this.opts.env,
      version: this.opts.version,
      currentScriptInfo: {
        name: path.basename(name), description: '', version: '',
        author: '', homepage: '', rawScript: code, scriptId: path.basename(name, '.js'),
      },
      utils: makeUtils(),
      on(evt, handler) { handlers[evt] = handler; },
      send(evt, data) {
        if (evt === 'inited') inited = data;
        if (evt === 'updateAlert' && self.opts.verbose) {
          console.log('[updateAlert] ' + JSON.stringify(data).slice(0, 160));
        }
      },
      request(url, options, cb) {
        if (self.opts.verbose) console.log('[req] ' + ((options || {}).method || 'GET') + ' ' + url);
        doRequest(url, options || {})
          .then(r => { self.reqLog.push({ url, status: r.statusCode }); cb(null, r); })
          .catch(err => cb(err, null));
      },
    };

    const sandbox = {
      console: { log: (...a) => self.opts.verbose && console.log('   ' + a.join(' ')),
        warn: () => {}, error: (...a) => self.opts.verbose && console.log('   ! ' + a.join(' ')),
        info: () => {}, debug: () => {}, group: () => {}, groupEnd: () => {}, trace: () => {} },
      Buffer, process, TextEncoder, TextDecoder, URL, URLSearchParams,
      crypto: nodeCrypto.webcrypto,
      fetch: () => Promise.reject(new Error('use lx.request')),
    };
    sandbox.globalThis = sandbox;
    sandbox.window = sandbox;
    sandbox.self = sandbox;
    sandbox.navigator = { userAgent: 'lx-music-mobile/' + this.opts.version, platform: 'Android' };
    sandbox.lx = lx;
    sandbox.globalThis.lx = lx;

    try {
      vm.runInNewContext(code, sandbox, { timeout: 20000, filename: name });
    } catch (e) {
      throw new Error('脚本加载失败: ' + (e && e.message || e));
    }

    if (!inited || !inited.sources) {
      throw new Error('脚本未声明 sources（可能被环境自检拒绝）');
    }

    const got = [];
    for (const [src, def] of Object.entries(inited.sources)) {
      this.scripts.set(src, {
        file: name, handler: handlers.request,
        qualitys: def.qualitys || [],
        actions: def.actions || [],
      });
      got.push({ source: src, actions: def.actions || [], qualitys: def.qualitys || [] });
    }
    // 记录脚本元信息（头部 @name/@version/@author 等注释）
    this.meta = parseMeta(code);
    this.scriptName = name;
    return got;
  }

  /** 加载一个音源脚本文件 */
  loadScript(file) {
    return this.loadCode(fs.readFileSync(file, 'utf8'), file);
  }

  /** 卸载全部音源 */
  clear() {
    this.scripts.clear();
    this.meta = null;
    this.scriptName = null;
  }

  /** 调音源脚本取直链 */
  async getMusicUrl(source, songId, quality) {
    const s = this.scripts.get(source);
    if (!s) throw new Error('未加载 source: ' + source + '（已加载: ' + [...this.scripts.keys()].join(',') + '）');
    if (!s.handler) throw new Error('该脚本没有注册 request 处理器');
    if (s.qualitys.length && !s.qualitys.includes(quality)) {
      quality = s.qualitys[s.qualitys.length - 1];   // 回落到最后（通常最高）可用音质
    }
    const musicInfo = { hash: songId, songmid: songId, songId, id: songId };
    return await s.handler({ action: 'musicUrl', source, info: { type: quality, musicInfo } });
  }

  /** 搜索 + 取链，返回 R1 侧契约 */
  async search(keyword, limit = 20, quality = '320k', source = null) {
    const items = await kuwoSearch(keyword, limit);
    const src = source || (this.scripts.has('kw') ? 'kw' : [...this.scripts.keys()][0]);
    const list = [];
    for (let i = 0; i < items.length; i++) {
      const it = items[i];
      let url = '';
      try { url = await this.getMusicUrl(src, it.rid, quality); } catch (e) { url = ''; }
      list.push({ index: i + 1, title: it.title, artist: it.artist, url: url || '' });
    }
    return { total: list.length, list };
  }
}

// ---------------------------------------------------------------- HTTP 网关
function serve(rt, port, quality) {
  const http = require('http');
  http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (obj, code = 200) => {
      const b = Buffer.from(JSON.stringify(obj, null, 0), 'utf8');
      res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': b.length });
      res.end(b);
    };
    if (u.pathname === '/search') {
      const k = u.searchParams.get('k') || '';
      const n = parseInt(u.searchParams.get('n') || '20', 10);
      const q = u.searchParams.get('q') || quality;
      rt.search(k, n, q).then(send).catch(e => send({ error: String(e && e.message || e) }, 500));
    } else if (u.pathname === '/health') {
      send({ ok: true, sources: [...rt.scripts.keys()] });
    } else {
      send({ error: 'not found' }, 404);
    }
  }).listen(port, '0.0.0.0', () => console.log('mini-lx gateway on 0.0.0.0:' + port));
}

// ---------------------------------------------------------------- CLI
async function main() {
  const argv = process.argv.slice(2);
  const cmd = argv[0];
  const help = () => console.log(
    '用法:\n' +
    '  node lxrt.js info   <script.js>\n' +
    '  node lxrt.js url    <script.js> <source> <songId> [quality]\n' +
    '  node lxrt.js search <关键词> [条数]\n' +
    '  node lxrt.js play   <script.js> <关键词> [quality]\n' +
    '  node lxrt.js serve  <script.js> [--port 8099]');

  if (!cmd) return help();

  if (cmd === 'search') {
    const r = await kuwoSearch(argv[1] || '', parseInt(argv[2] || '20', 10));
    console.log(JSON.stringify({ total: r.length, list: r }, null, 2));
    return;
  }
  if (cmd === 'info') {
    const rt = new LXRuntime();
    console.log(JSON.stringify(rt.loadScript(argv[1]), null, 2));
    return;
  }
  if (cmd === 'url') {
    const rt = new LXRuntime();
    rt.loadScript(argv[1]);
    const url = await rt.getMusicUrl(argv[2], argv[3], argv[4] || '320k');
    console.log(url);
    return;
  }
  if (cmd === 'play' || cmd === 'serve') {
    if (cmd === 'play') {
      const rt = new LXRuntime();
      rt.loadScript(argv[1]);
      const r = await rt.search(argv[2] || '', parseInt(argv[4] || '5', 10), argv[3] || '320k');
      console.log(JSON.stringify(r, null, 2));
    } else {
      const pIdx = argv.indexOf('--port');
      const port = pIdx > -1 ? parseInt(argv[pIdx + 1], 10) : 8099;
      const rt = new LXRuntime({ verbose: false });
      rt.loadScript(argv[1]);
      serve(rt, port, argv[2] || '320k');
    }
    return;
  }
  help();
}

if (require.main === module) {
  main().catch(e => { console.error('ERR: ' + (e && e.message || e)); process.exit(1); });
}

module.exports = { LXRuntime, kuwoSearch, doRequest };
