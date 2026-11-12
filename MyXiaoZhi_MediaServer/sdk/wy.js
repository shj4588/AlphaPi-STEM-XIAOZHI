'use strict';
/**
 * wy.js —— 落雪 musicSdk 网易云（wy）模块的 Node 移植
 * 移植自 src/utils/musicSdk/wy/{musicSearch.js, utils/index.js, utils/crypto.js}
 *
 * 加密：eapi（与 NeteaseCloudMusicApi 同源）
 *   message = `nobody${url}use${text}md5forencrypt`
 *   digest  = MD5(message)
 *   data    = `${url}-36cd479b6b5-${text}-36cd479b6b5-${digest}`
 *   params  = HEX_UPPER(AES_ECB_PKCS7(base64(data), key='e82ckenh8dichen8'))
 * POST http://interface.music.163.com/eapi/batch  (application/x-www-form-urlencoded)
 */
const { httpFetch } = require('./http');
const { decodeName, formatPlayTime, formatSingerName, artistMatch } = require('./util');
const { AES_MODE, aesEncryptSync, md5 } = require('./crypto');

const EAPI_KEY = 'e82ckenh8dichen8';
const UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0.3112.90 Safari/537.36';

// ---------------------------------------------------------------------------
// ★★ 登录态 Cookie（解锁 VIP / 无版权曲目）
//
// 这就是「为什么落雪音源能播原版、我们只能播翻唱」的答案：
//   六音音源脚本头写着 `@netease MUSIC_U=;` `@tencent ts_last=...`
//   ⇒ 它请求的是平台官方接口，靠**用户登录 cookie** 取 VIP 曲的完整直链。
//   不带登录态时，网易对周杰伦/陈奕迅等返回 `code=-110`（需付费/无权限），
//   我们只能退而求其次播别人唱的同名版本。
//
// 填入自己的 MUSIC_U 后，同一首歌会直接给出 320k 原版直链。
// 来源优先级：setCookie() > 环境变量 WY_COOKIE > files/wy_cookie
// ---------------------------------------------------------------------------
const fs = require('fs');
const path = require('path');
let _cookie = '';

function setCookie(v) { _cookie = String(v || '').trim(); }
function getCookie() {
  if (_cookie) return _cookie;
  const env = (process.env.WY_COOKIE || '').trim();
  if (env) return env;
  try {
    const f = path.join(__dirname, '..', 'files', 'wy_cookie');
    if (fs.existsSync(f)) return fs.readFileSync(f, 'utf8').trim();
  } catch { /* 没有就用匿名 */ }
  return '';
}
/** 组装请求头（有登录态就带上 Cookie） */
function wyHeaders(extra) {
  const h = Object.assign({ 'User-Agent': UA, Referer: 'https://music.163.com/' }, extra || {});
  const c = getCookie();
  if (c) h.Cookie = c;
  return h;
}
const hasCookie = () => !!getCookie();

// wy/utils/crypto.js eapi
function eapi(url, object) {
  const text = typeof object === 'object' ? JSON.stringify(object) : object;
  const digest = md5(`nobody${url}use${text}md5forencrypt`);
  const data = `${url}-36cd479b6b5-${text}-36cd479b6b5-${digest}`;
  const b64 = aesEncryptSync(Buffer.from(data).toString('base64'), EAPI_KEY, '', AES_MODE.ECB_128_NoPadding);
  return { params: Buffer.from(b64, 'base64').toString('hex').toUpperCase() };
}

// wy/utils/index.js eapiRequest
async function eapiRequest(url, data) {
  const { body } = await httpFetch('http://interface.music.163.com/eapi/batch', {
    method: 'post',
    headers: { 'User-Agent': UA, origin: 'https://music.163.com' },
    form: eapi(url, data),
  });
  return body;
}

/**
 * 通道 A（忠实移植）：eapi  /api/search/song/list/page
 * 通道 B（兜底）    ：明文  https://music.163.com/api/search/get
 *
 * ★ 实测：eapi 的 POST /eapi/batch 在部分网络下返回 HTTP 200 + 空 body（风控/缺登录态），
 *   而明文接口稳定可用 ⇒ 保留 eapi 作为首选，空结果时自动降级到 B，保证功能不瘫。
 */
async function searchEapi(keyword, limit, page) {
  const body = await eapiRequest('/api/search/song/list/page', {
    keyword,
    needCorrect: '1',
    channel: 'typing',
    offset: limit * (page - 1),
    scene: 'normal',
    total: page === 1,
    limit,
  });
  if (!body || body.code !== 200) return [];
  const raw = (body.data && body.data.resources) || [];
  const out = [];
  for (const r of raw) {
    const it = r && r.baseInfo && r.baseInfo.simpleSongData;
    if (!it) continue;
    out.push({
      rid: String(it.id),
      title: decodeName(it.name),
      artist: decodeName(formatSingerName(it.ar, 'name')),
      album: decodeName(it.al && it.al.name || ''),
      duration: it.dt ? formatPlayTime(it.dt / 1000) : '',
      img: (it.al && it.al.picUrl) || '',
      source: 'wy',
      via: 'eapi',
      types: [],
    });
  }
  return out;
}

async function searchPlain(keyword, limit, page) {
  const url = 'https://music.163.com/api/search/get' +
    `?s=${encodeURIComponent(keyword)}&type=1&limit=${limit}&offset=${limit * (page - 1)}`;
  const { body } = await httpFetch(url, { headers: wyHeaders({ referer: 'https://music.163.com/' }) });
  if (!body || body.code !== 200 || !body.result) return [];
  const songs = body.result.songs || [];
  return songs.map(s => ({
    rid: String(s.id),
    title: decodeName(s.name || ''),
    artist: decodeName(formatSingerName(s.artists, 'name')),
    album: decodeName((s.album && s.album.name) || ''),
    duration: s.duration ? formatPlayTime(s.duration / 1000) : '',
    img: (s.album && s.album.picUrl) || '',
    source: 'wy',
    via: 'plain',
    types: [],
  }));
}

async function search(keyword, limit = 30, page = 1) {
  let list = [];
  try { list = await searchEapi(keyword, limit, page); } catch { list = []; }
  if (!list.length) {
    try { list = await searchPlain(keyword, limit, page); } catch (e) {
      throw new Error('网易搜索失败: ' + String(e && e.message || e).slice(0, 80));
    }
  }
  return list.slice(0, limit);
}

// ---------------------------------------------------------------- 内置直链
/**
 * ★ 网易云内置取链（实测 2026-10-15 可用，出完整 320k 直链，无需 cookie/加密）：
 *   `https://music.163.com/api/song/enhance/player/url?id=<id>&ids=[<id>]&br=<br>`
 *   → `{data:[{id,url,br,size,...}]}`。br: 128000/192000/320000（越高越可能因版权返回 null）。
 *   注意：这是**官方明文 API**，eapi 通道实测返 200 空 body（见 search 的双通道）。
 */
async function directUrl(id, br = 320000) {
  for (const b of [br, 320000, 128000]) {
    try {
      const r = await httpFetch(
        'https://music.163.com/api/song/enhance/player/url?id=' + encodeURIComponent(id) +
        '&ids=' + encodeURIComponent(JSON.stringify([String(id)])) + '&br=' + b,
        { timeout: 15000, headers: wyHeaders() });
      const d = (r.body && r.body.data) || [];
      const it = d[0];
      if (it && it.url) return it.url;
    } catch { /* 抖动 → 下一档 */ }
  }
  return '';
}

/**
 * ★ 批量取链：一次请求拿多个 id 的直链。
 *   实测 `ids=[a,b,c,...]` 会返回每个 id 的结果（无版权者 url=null，code=-110）。
 *   必要性：跨源要试十来个候选，逐条请求要十几秒；批量后 1 次搞定。
 *   @returns {Map<string,{url,br,size}>}
 */
async function batchDirectUrl(ids, br = 320000) {
  const out = new Map();
  const list = [...new Set(ids.map(String))].filter(Boolean);
  if (!list.length) return out;
  try {
    const r = await httpFetch(
      'https://music.163.com/api/song/enhance/player/url?id=' + encodeURIComponent(list[0]) +
      '&ids=' + encodeURIComponent(JSON.stringify(list)) + '&br=' + br,
      { timeout: 15000, headers: wyHeaders() });
    for (const it of (r.body && r.body.data) || []) {
      if (it && it.url) out.set(String(it.id), { url: it.url, br: it.br, size: it.size });
    }
  } catch { /* 抖动 → 空表，交给调用方降级 */ }
  return out;
}

// 归一化：去空格、去括号及之后内容（「孤勇者 (Live)」→「孤勇者」）
const norm = s => String(s || '').replace(/\s/g, '').replace(/[（(].*$/, '').toLowerCase();

/**
 * 版本质量惩罚表。
 * 原曲无版权时，网易里能播的往往是各种二创版本，不排序的话会挑到 DJ/纯音乐/铃声，
 * 用户点「周杰伦 晴天」结果听到一段电音 —— 比听到提示音更莫名其妙。
 * 因此：器乐/电音类重罚（基本不会选中），人声翻唱轻罚（可用，排在后面）。
 */
const BAD_VER = /(dj|remix|电音|纯音乐|伴奏|钢琴|吉他|古筝|二胡|小提琴|萨克斯|卡点|铃声|闹钟|清唱|哼唱|童声|宝宝|娃娃|催眠|ambient)/i;
const MEH_VER = /(翻唱|cover|原唱|深情版|女声版|男声版|烟嗓|女生版|男生版|合唱|群星)/i;

/**
 * ★ 跨源匹配：拿「歌名 + 歌手」去网易云找可播的一首，并**分级**返回。
 *
 * 为什么分级：网易对整批无版权曲目一律返回 url=null（实测陈奕迅全部 null，
 * 而同名翻唱/其它版本有 url）。若死守「必须歌手一致」，这些歌就落到酷我
 * 11 秒试听片段（用户听到「当前音乐仅在酷我音乐客户端播放」）。
 * 因此：
 *   kind='exact' —— 歌名+歌手都对得上，原曲
 *   kind='alt'   —— 只有歌名对得上（同名其它版本/翻唱），至少是完整曲
 * 由上层决定是否接受 alt（默认接受，但要在 UI 上标注）。
 *
 * @param {{exactOnly?:boolean}} opt  exactOnly=true 时只要原唱（找不到就返回空）
 * @returns {{url:string, kind:'exact'|'alt'|'', br:number, size:number, song:{rid,title,artist}|null}}
 */
async function matchUrl(title, artist, opt = {}) {
  const empty = { url: '', kind: '', br: 0, size: 0, song: null };
  if (!title) return empty;

  // 1) 两轮搜索：带歌手（准）+ 纯歌名（全），合并去重
  const cands = new Map();
  const kws = [];
  if (artist) kws.push(artist + ' ' + title);
  kws.push(title);
  for (const kw of kws) {
    try {
      for (const x of await search(kw, 8)) cands.set(String(x.rid), x);
    } catch { /* 单轮失败不致命 */ }
  }
  if (!cands.size) return empty;

  // 2) 打分：歌名 0/1/3，歌手对不上 +2，版本类型再加减
  const t = norm(title);
  const a = norm(artist);
  const scored = [...cands.values()].map(x => {
    const nt = norm(x.title);
    const raw = String(x.title || '');
    let s = nt === t ? 0 : (nt.includes(t) || t.includes(nt) ? 1 : 3);
    if (a) {
      // ★ 用 artistMatch 而不是裸字符串包含：网易常只标英文名（「晴天 — Jay」），
      //   裸包含会把原唱判成翻唱（2026-10-15 实测踩到）。
      if (!artistMatch(artist, x.artist)) s += 2;
      else if (nt.includes(a)) s -= 1;         // 歌名里写明「(原唱 周杰伦)」：算半个原唱
    }
    if (BAD_VER.test(raw)) s += 6;             // DJ/纯音乐/铃声等：基本不选
    else if (MEH_VER.test(raw)) s += 1;        // 人声翻唱：可用，但排后面
    return { x, s };
  }).sort((p, q) => p.s - q.s);

  // 3) 批量取链。★ 顺序必须是「先挑对人，再挑音质」：
  //    先只用 320k 找歌手对得上的（exact）；找不到再拉 128k 补一轮。
  //    （实测反例：周杰伦《晴天》320k 无版权、128k 有；若按「谁有 url 先拿谁」，
  //     会挑到翻唱版 —— 歌是能播了，但用户点周杰伦听到陌生人唱。）
  const top = scored.slice(0, 12);
  const ids = top.map(o => o.x.rid);
  const pack = (o, hit) => ({
    url: hit.url,
    kind: o.s <= 1 ? 'exact' : 'alt',
    br: hit.br || 0,
    size: hit.size || 0,
    song: { rid: String(o.x.rid), title: o.x.title, artist: o.x.artist },
  });

  const find = (avail, exactOnly) => {
    for (const o of top) {
      if (exactOnly && o.s > 1) continue;      // exactOnly = 只要歌手对得上的
      const hit = avail.get(String(o.x.rid));
      if (hit) return pack(o, hit);
    }
    return null;
  };

  let loMap = null;
  const lo = async () => (loMap || (loMap = await batchDirectUrl(ids, 128000)));  // 懒加载，只请求一次
  const hi = await batchDirectUrl(ids, 320000);
  const got = find(hi, true)              // ① 320k 里的原唱
    || find(await lo(), true)             // ② 320k 没原唱 → 降 128k 再找一次原唱
    || (opt.exactOnly ? null : find(hi, false))      // ③ 原唱全网无版权 → 320k 同名版本
    || (opt.exactOnly ? null : find(await lo(), false)); // ④ 最后：128k 同名版本
  return got || empty;
}

/** 兼容旧签名：只要 url（优先 exact，其次 alt） */
async function directUrlByMeta(title, artist) {
  const r = await matchUrl(title, artist);
  return r.url || '';
}

module.exports = {
  search, eapi, eapiRequest, directUrl, batchDirectUrl, directUrlByMeta, matchUrl,
  // ★ 登录态 cookie（填自己的 MUSIC_U 才能取 VIP / 无版权曲的原版直链）
  setCookie, getCookie, hasCookie,
};
