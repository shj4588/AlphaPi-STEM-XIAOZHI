'use strict';
/**
 * lxrelay.js —— lxserver（XCQ0607/lxserver）取链中继。
 *
 * 背景：lxserver 是 LX Music Sync Server 增强版的 docker 镜像
 * （`xcq0607/lxserver:latest`，默认端口 9527），服务端跑的是
 * lx-music-desktop 原版 musicSdk，取链统一走「自定义源脚本（订阅源）」——
 * 桌面版能用的订阅源它都能用，比我们自己维护的五级兜底面广。
 *
 * 契约（扒源码 src/server/server.ts '/api/music/url' 确认）：
 *   POST {base}/api/music/url
 *   Content-Type: application/json
 *   {"songInfo": {"source":"kw","songmid":"123", ...}, "quality":"320k",
 *    "enableAutoSwitchApiSource": true}
 *   → {"success":true, "url":"https://...", ...}
 * 鉴权：不带 x-user-name 就是公开用户 'open'，免密码（局域网自用刚好）。
 * 302：它服务端会自己把跳转解析成直链（≤3 跳），我们拿到后照旧再验一次真。
 *
 * 启用：在 mini-lx 目录写 files/my_lx，内容为 lxserver 地址（如
 *   http://192.168.2.189:9527
 * 删掉该文件即整体回退（relayUrl 直接返回 {skip:true}）。
 *
 * 可选第二行：`用户名:Token`（lxserver 后台给用户建的持久化 API Token）。
 * 匿名（open）用户本就能用「公开源」取链，但若后台开了
 * ENABLE_PUBLIC_USER_RESTRICTION 或源被设为非公开，就需要这一行：
 *   http://192.168.2.189:9527
 *   r1:e3b1c0d2f4a6
 *
 * 搜索契约（2026-10-18 实测确认）：
 *   GET {base}/api/music/search?source=kw&name=孙燕姿&limit=20
 *   → {"list":[{"name","singer","source","songmid","albumName","interval","img",
 *               "_types":{"128k":{"size","hash"},"320k":{...},"flac":{...}}}]}
 *   ★ 参数名是 **name**（keyword / key 都会回 "Missing name"）；POST 这个路径是 404。
 *   ★★ **普通搜索必须带 limit**：不带的话实测一次吐 3600 条（它把全部匹配都塞回来）。
 *      —— 但 2026-10-31 起 ab（有声书）路径反过来**利用**这一点：limit:'all' 就
 *      故意不带 limit，一次拿整本书全量（凡人修仙传第一季 = 2101 集，8~9s）。
 *   ★★ 返回里的整条 songInfo（尤其 `_types`）要**原样回传**给 /api/music/url：
 *      实测 mg 只给 songmid 会 500「咪咕找不到」，带上它自己的 _types 就 200 出真链。
 *
 * ★ 只负责「拿链 / 搜歌」，不碰验真/302/缓存/兜底——那些都在 server.js 的
 *   resolveUrl() 里按原有风格做，本模块保持无状态、可单测。
 */
const fs = require('fs');
const path = require('path');

const FLAG = path.join(__dirname, 'files', 'my_lx');
const TIMEOUT = 12000;
/**
 * 搜索的超时比取链短：五源并发各一次，搜不出来要赶紧回落脚本音源
 * （最坏 6s + 脚本源 2~3s ≈ 9s，还在 device 搜歌 15s 的预算内）。
 */
const SEARCH_TIMEOUT = 6000;

/** lxserver 地址；files/my_lx 不存在 = 未启用（返回 null）。只取第一行。 */
function base() {
  try {
    if (!fs.existsSync(FLAG)) return null;
    // ★ 只取第一行：flag 支持两行（第二行 user:token），整读会把第二行拼进 URL
    const s = fs.readFileSync(FLAG, 'utf8').split(/\r?\n/)[0].trim().replace(/\/+$/, '');
    return /^https?:\/\//.test(s) ? s : null;
  } catch {
    return null;
  }
}

/**
 * 可选认证：files/my_lx 第二行 `用户名:Token`。
 * lxserver 的 verifyUserAuth 只认 x-user-token（用户名+密码的明文校验被官方
 * 注释掉了 ⇒ 具名用户必须在后台建持久化 API Token 带走这个头）。
 * @returns {{user:string, token:string}|null}
 */
function auth() {
  try {
    if (!fs.existsSync(FLAG)) return null;
    const lines = fs.readFileSync(FLAG, 'utf8').split(/\r?\n/)
      .map(s => s.trim()).filter(Boolean);
    if (lines.length < 2) return null;
    const i = lines[1].indexOf(':');
    if (i <= 0) return null;
    return { user: lines[1].slice(0, i), token: lines[1].slice(i + 1).trim() };
  } catch {
    return null;
  }
}

/**
 * 向 lxserver 要一条直链。
 *
 * @param {{source:string, rid:string, title?:string, artist?:string, quality?:string,
 *          info?:object}} opt
 *        info = 落雪搜索返回的**整条 songInfo**（含 _types）。有就原样带上 ——
 *              实测 mg 缺了它是 500，带上就 200 出真链。
 * @returns {Promise<{url:string, ms:number}|{err:string, ms:number}|{skip:true}>}
 *          url  = 拿到直链（调用方仍需 probeUrl 验真）
 *          err  = 对方明确说不行（会记入 debug.steps；是否负缓存由调用方定）
 *          skip = 未启用 / 参数不全（静默，不记失败）
 */
async function relayUrl(opt) {
  const b = base();
  if (!b) return { skip: true };
  const o = opt || {};
  if (!o.source || !o.rid) return { skip: true };
  // lx 标准 songInfo：kw/wy/tx/mg 认 songmid；kg 的「歌曲 id」在 musicSdk 里
  // 就是 hash（取链还要 _types[quality].hash，那只有搜索结果才带——缺了就
  // 算失败，落回我们自己的兜底，见 server.js ⓪ 级注释）。
  const songInfo = Object.assign({}, o.info || {});
  songInfo.source = o.source;
  songInfo.songmid = o.rid;
  songInfo.id = o.rid;
  // ★ kg 的顶层 hash 与 songmid 不是一回事（真 hash 在 _types[quality] 里），
  //   所以只在 info 没给 hash 时才用 rid 顶上，别把搜索给的覆盖掉。
  if (!songInfo.hash) songInfo.hash = o.rid;
  if (!songInfo.name) songInfo.name = o.title || '';
  if (!songInfo.singer) songInfo.singer = o.artist || '';
  const t0 = Date.now();
  try {
    const h = { 'Content-Type': 'application/json', 'User-Agent': 'Mozilla/5.0' };
    const au = auth();
    if (au) {
      h['x-user-name'] = au.user;
      h['x-user-token'] = au.token;
    }
    const r = await fetch(b + '/api/music/url', {
      method: 'POST',
      headers: h,
      body: JSON.stringify({
        songInfo,
        quality: o.quality || '320k',
        enableAutoSwitchApiSource: true,
      }),
      signal: AbortSignal.timeout(TIMEOUT),
    });
    const txt = await r.text();
    let j = null;
    try {
      j = JSON.parse(txt);
    } catch {
      /* 非 JSON（代理页/HTML）按失败处理 */
    }
    if (!r.ok) {
      return { err: 'HTTP ' + r.status + ' ' + String(txt).slice(0, 120), ms: Date.now() - t0 };
    }
    const url = j && j.url ? String(j.url) : '';
    if (!url) {
      return {
        err: (j && (j.message || j.error)) || '返回里没有 url 字段: ' + String(txt).slice(0, 120),
        ms: Date.now() - t0,
      };
    }
    return { url, ms: Date.now() - t0 };
  } catch (e) {
    return { err: String((e && e.message) || e).slice(0, 140), ms: Date.now() - t0 };
  }
}

/**
 * 向 lxserver 搜歌（**取链同源** ⇒ 搜到的 rid 它一定认）。
 *
 * @param {{source?:string, name:string, limit?:number}} opt
 * @returns {Promise<{list:object[], ms:number}|{err:string, ms:number}|{skip:true}>}
 *          list = 原始条目（保留 _types 等全部字段，调用方自己映射 / 暂存）
 */
async function search(opt) {
  const b = base();
  if (!b) return { skip: true };
  const o = opt || {};
  const name = String(o.name || o.keyword || '').trim();
  if (!name) return { skip: true };
  const src = String(o.source || 'kw').trim();
  // ★ limit:'all'（或 0）= 故意**不带** limit ⇒ lxserver 一次吐全量匹配。
  //   只有 ab（有声书整本枚举）用它：实测凡人修仙传第一季 3601 条 / 1.6MB / ~9s。
  const wantAll = (o.limit === 'all' || o.limit === 0);
  const lim = wantAll ? 0 : Math.min(Math.max(parseInt(o.limit, 10) || 20, 1), 100);
  // ★ page 参数（2026-10-31 晚实测：落雪搜索**认** page —— page=1/2 零重叠，
  //   之前“不认 page”的结论是错的）。有声书整本枚举就靠它翻页。
  const page = Math.max(parseInt(o.page, 10) || 1, 1);
  const t0 = Date.now();
  try {
    const h = { 'User-Agent': 'Mozilla/5.0' };
    const au = auth();
    if (au) {
      h['x-user-name'] = au.user;
      h['x-user-token'] = au.token;
    }
    const u = b + '/api/music/search?source=' + encodeURIComponent(src)
      + '&name=' + encodeURIComponent(name) + (lim ? '&limit=' + lim : '')
      + (page > 1 ? '&page=' + page : '');
    // ★ 全量模式上游要现算几千条，实测 9s 上下 ⇒ 超时放宽到 30s；
    //   普通 100 条搜索维持 6s 短超时（搜不出来赶紧回落脚本音源）。
    const T = wantAll ? 30000 : SEARCH_TIMEOUT;
    const r = await fetch(u, { headers: h, signal: AbortSignal.timeout(T) });
    const txt = await r.text();
    let j = null;
    try {
      j = JSON.parse(txt);
    } catch { /* 非 JSON 按失败处理 */ }
    if (!r.ok) {
      return { err: 'HTTP ' + r.status + ' ' + String(txt).slice(0, 120), ms: Date.now() - t0 };
    }
    const list = (j && Array.isArray(j.list)) ? j.list : [];
    return { list, ms: Date.now() - t0 };
  } catch (e) {
    return { err: String((e && e.message) || e).slice(0, 140), ms: Date.now() - t0 };
  }
}

/**
 * ★ 向 lxserver 要歌词（2026-10-18 新增）。
 *
 * 为什么需要它：server.js 的 /api/lyric 以前只会拿 rid 去问**酷我直连**
 * （sdk/kw.js 的 m.kuwo.cn 接口），于是
 *   · wy / kg / mg 的 rid 喂给酷我 ⇒ 必然 0 行（rid 是别家的 ID）；
 *   · 部分 kw rid 酷我那个接口本身就没词（实测「稻香」kw:440613：
 *     酷我 0 行、落雪 1122 字节）。
 * 落雪的 /api/music/lyric 是**多源**的（source 指哪家就查哪家），
 * 所以歌词也必须像取链一样中继一次。
 *
 * 契约（实测确认）：GET {base}/api/music/lyric?source=kw&songmid=<rid>
 *   → {"lyric":"[ti:..]\n[00:00.000].."}  （注意是 GET，POST 该路径 404）
 *
 * @param {{source:string, rid:string}} opt
 * @returns {Promise<{lines:Array<{time:number,text:string}>, lrc:string, ms:number}
 *                   |{err:string, ms:number}|{skip:true}>}
 */
async function relayLyric(opt) {
  const b = base();
  if (!b) return { skip: true };
  const o = opt || {};
  if (!o.source || !o.rid) return { skip: true };
  const t0 = Date.now();
  try {
    const h = { 'User-Agent': 'Mozilla/5.0' };
    const au = auth();
    if (au) {
      h['x-user-name'] = au.user;
      h['x-user-token'] = au.token;
    }
    const url = b + '/api/music/lyric?source=' + encodeURIComponent(o.source)
      + '&songmid=' + encodeURIComponent(o.rid);
    const r = await fetch(url, { headers: h, signal: AbortSignal.timeout(TIMEOUT) });
    const txt = await r.text();
    let j = null;
    try {
      j = JSON.parse(txt);
    } catch {
      /* 非 JSON（代理页/HTML）按失败处理 */
    }
    if (!r.ok) {
      return { err: 'HTTP ' + r.status + ' ' + String(txt).slice(0, 120), ms: Date.now() - t0 };
    }
    const raw = (j && typeof j.lyric === 'string') ? j.lyric : '';
    const lines = parseLrc(raw);
    if (!lines.length) {
      return { err: '落雪没给词（lyric ' + raw.length + ' 字节）', ms: Date.now() - t0 };
    }
    return { lines, lrc: raw, ms: Date.now() - t0 };
  } catch (e) {
    return { err: String((e && e.message) || e).slice(0, 140), ms: Date.now() - t0 };
  }
}

/**
 * LRC 文本 → [{time(秒), text}]。
 * 只认 [mm:ss.xx] / [mm:ss.xxx] 时间标签，其余元信息行（[ti:] [ar:] [al:] …）丢掉。
 */
function parseLrc(raw) {
  const out = [];
  if (!raw) return out;
  const re = /\[(\d{1,3}):(\d{1,2}(?:[.:]\d{1,3})?)\]/g;
  const lines = String(raw).split(/\r?\n/);
  for (const ln of lines) {
    const m = re.exec(ln);
    re.lastIndex = 0;                     // ★ 复用带 g 的正则必须手动归零
    if (!m) continue;
    const t = parseFloat(m[1]) * 60 + parseFloat(String(m[2]).replace(':', '.'));
    const text = ln.slice(m.index + m[0].length).trim();
    if (!isNaN(t) && text) out.push({ time: t, text });
  }
  out.sort((a, b) => a.time - b.time);
  return out;
}

module.exports = { base, auth, relayUrl, search, relayLyric, parseLrc };

