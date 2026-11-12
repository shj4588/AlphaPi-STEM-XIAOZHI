'use strict';
/**
 * kg.js —— 落雪 musicSdk 酷狗（kg）模块的 Node 移植
 * 移植自 src/utils/musicSdk/kg/musicSearch.js
 *
 * 特点：★ 无任何签名，纯 GET 公开接口 —— 五个源里最干净的一个。
 */
const { httpFetch } = require('./http');
const { decodeName, formatPlayTime, formatSingerName, artistMatch } = require('./util');

const SEARCH_URL = 'https://songsearch.kugou.com/song_search_v2' +
  '?keyword={kw}&page={page}&pagesize={limit}&userid=0&clientver=&platform=WebFilter' +
  '&filter=2&iscorrection=1&privilege_filter=0&area_code=1';

// 对应 kg/musicSearch.js filterData
function filterData(d) {
  const types = [];
  if (d.FileSize) types.push({ type: '128k', hash: d.FileHash });
  if (d.HQFileSize) types.push({ type: '320k', hash: d.HQFileHash });
  if (d.SQFileSize) types.push({ type: 'flac', hash: d.SQFileHash });
  if (d.ResFileSize) types.push({ type: 'flac24bit', hash: d.ResFileHash });
  return {
    rid: String(d.Audioid || ''),
    title: decodeName(`${d.OriSongName || ''}${d.Suffix ? ' ' + d.Suffix : ''}`),
    artist: decodeName(formatSingerName(d.Singers, 'name')),
    album: decodeName(d.AlbumName || ''),
    duration: d.Duration ? formatPlayTime(d.Duration) : '',
    img: '',
    source: 'kg',
    hash: d.FileHash || '',
    // 取链用：音质从高到低试（SQ=flac / HQ=320k / FileHash=128k）
    hqhash: (d.HQFileHash || '').trim(),
    sqhash: (d.SQFileHash || '').trim(),
    types,
  };
}

async function search(keyword, limit = 30, page = 1) {
  const url = SEARCH_URL
    .replace('{kw}', encodeURIComponent(keyword))
    .replace('{page}', String(page))
    .replace('{limit}', String(limit));
  const { body } = await httpFetch(url);
  if (!body || body.error_code !== 0) return [];
  const raw = (body.data && body.data.lists) || [];
  const seen = new Set();
  const out = [];
  for (const item of raw) {
    const key = String(item.Audioid) + item.FileHash;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(filterData(item));
    // 同曲多版本（Grp）
    for (const child of (item.Grp || [])) {
      const k2 = String(child.Audioid) + child.FileHash;
      if (seen.has(k2)) continue;
      seen.add(k2);
      out.push(filterData(child));
    }
  }
  return out.filter(x => x.rid).slice(0, limit);
}

// ---------------------------------------------------------------------------
// ★ 取链：酷狗 playInfo（搜索 m.kugou.com，hash → backup_url）
//   决定性实测（2026-10-15）：这台机/NAS 的网络上第三方音源后端全部不可达，
//   但平台官方域名直连全通。其中酷狗 playInfo 是唯一稳定给出**完整原曲**的通道：
//   周杰伦《晴天》在酷我只给 181,521 字节（11 秒试听提示音），
//   酷狗 backup_url 给 3.3MB 完整曲，6/6 命中。
// ---------------------------------------------------------------------------
const KG_UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 14_0 like Mac OS X) AppleWebKit/605.1.15';

// ★★ 限流防护（实测踩到）：酷狗 playInfo 对同一出口 IP 有频率限制，
//    触发后返回 {"errcode":1002,"error":"您操作太频繁了，请稍后再试"}，
//    且熔断期内**所有** hash 都失败（曾导致 6/6 命中瞬间变 0/6）。
//    对策：① 全局串行队列 + 最小间隔；② 命中 1002 立刻进入冷却期，冷却内直接放弃（不再堆请求）。
const MIN_GAP = 1200;        // 两次 playInfo 之间至少 1.2s
const COOLDOWN = 60000;      // 命中限流后冷却 60s
let _last = 0;
let _coolUntil = 0;
let _chain = Promise.resolve();

function throttled(fn) {
  _chain = _chain.then(async () => {
    if (Date.now() < _coolUntil) return null;               // 冷却中，别再打
    const wait = MIN_GAP - (Date.now() - _last);
    if (wait > 0) await new Promise(r => setTimeout(r, wait));
    _last = Date.now();
    return fn();
  }).catch(() => null);
  return _chain;
}

/** 取一个 hash 的可播直链（backup_url 优先，play_url 兜底） */
async function playInfo(hash) {
  if (!hash) return '';
  if (Date.now() < _coolUntil) return '';                   // 冷却期直接放弃
  const d = await throttled(async () => {
    try {
      const r = await httpFetch('https://m.kugou.com/app/i/getSongInfo.php?cmd=playInfo&hash=' +
        encodeURIComponent(hash), { timeout: 12000, headers: { 'User-Agent': KG_UA } });
      return r.body || {};
    } catch { return {}; }
  }) || {};
  // ★ 限流熔断：命中后整条链在冷却期内都不再请求，避免把配额彻底打光
  if (d.errcode === 1002 || /操作太频繁/.test(String(d.error || ''))) {
    _coolUntil = Date.now() + COOLDOWN;
    return '';
  }
  if (d.errcode != null && d.errcode !== 0) return '';
  const urls = [];
  if (Array.isArray(d.backup_url)) for (const u of d.backup_url) urls.push(String(u));
  if (d.play_url) urls.push(String(d.play_url));
  if (d.url) urls.push(String(d.url));
  return urls.find(u => /^https?:\/\//.test(u)) || '';
}

/**
 * 顺序实测结论：SQ(flac) / HQ(320k) 的 hash 对 VIP 曲目常被拒，
 * 而普通 FileHash(128k) 出链率最高 ⇒ 先试 FileHash，最多试 2 个（省配额）。
 */
async function directUrl(item) {
  if (!item) return '';
  let tried = 0;
  for (const h of [item.hash, item.hqhash, item.sqhash]) {
    if (!h || tried >= 2) continue;
    tried++;
    const u = await playInfo(h);
    if (u) return u;
  }
  return '';
}

/** 版本号关键词 → 惩罚分（越高越不像原唱录音室版） */
const VER_RULES = [
  [/伴奏|纯音乐|instrumental|inst\.|无人声|karaoke|消音/i, 16],
  [/片段|30秒|试听|铃声|preview|铃声版/i, 14],
  [/翻唱|cover|网友改编|粉丝合唱|改编版|别人/i, 11],
  [/dj|remix|电音|合成|electro/i, 9],
  [/live|现场|演唱会|音乐会|巡演|音乐节|综艺|节目|cCTV|中国好声音|歌手/i, 4],
  [/钢琴|吉他|小提琴|古筝|二胡|清唱|acapella|八音盒|口琴|翻弹/i, 7],
];
function verPenalty(name) {
  let p = 0;
  for (const [re, w] of VER_RULES) if (re.test(name)) p += w;
  return p;
}

/**
 * ★ 跨源匹配：拿「歌名 + 歌手」在酷狗找最像原唱的一首并取直链。
 * @returns {{url:string, kind:'exact'|'alt'|'', song:{title,artist,hash}|null}}
 */
async function matchUrl(title, artist) {
  const empty = { url: '', kind: '', song: null };
  if (!title) return empty;
  // 两轮搜索：带歌手更准；带歌手没结果时退回只搜歌名
  const cands = new Map();
  for (const kw of [artist ? artist + ' ' + title : title, title]) {
    try {
      const l = await search(kw, 20);
      for (const x of l) if (x.title) cands.set(x.hash || x.rid, x);
    } catch { /* 抖动 */ }
    if (cands.size >= 8) break;
  }
  if (!cands.size) return empty;

  const norm = s => String(s || '').replace(/\s|<[^>]*>/g, '').toLowerCase();
  const stem = s => norm(s).replace(/[（(].*$/, '');
  const t = stem(title);
  const a = norm(artist);

  const scored = [...cands.values()].map(x => {
    const nt = stem(x.title);
    let s = nt === t ? 0 : (nt.includes(t) || t.includes(nt) ? 3 : 12);   // 歌名
    if (a) {
      // 与网易共用别名匹配（「周杰伦」≡「Jay」）
      if (!artistMatch(artist, x.artist)) s += 8;                           // 歌手对不上
    }
    s += verPenalty(x.title + ' ' + x.album);                              // 版本号惩罚
    if (x.sqhash || x.hqhash) s -= 1;                                      // 有高音质 → 略优先
    return { x, s };
  }).sort((p, q) => p.s - q.s);

  // 按分数从低到高逐个试。★ 只试前 4 个：playInfo 有配额，多试一个就少一分余量；
  // 一旦进入冷却期（限流）立刻停手，不要继续消耗。
  for (const o of scored.slice(0, 4)) {
    if (Date.now() < _coolUntil) break;
    const url = await directUrl(o.x);
    if (!url) continue;
    return {
      url,
      kind: o.s <= 3 ? 'exact' : 'alt',
      song: { title: o.x.title, artist: o.x.artist, hash: o.x.hash, source: 'kg' },
    };
  }
  return empty;
}

module.exports = {
  search, playInfo, directUrl, matchUrl,
  /** 冷却期剩余毫秒（>0 表示正被酷狗限流，UI 可提示"稍后再试"） */
  cooldownLeft: () => Math.max(0, _coolUntil - Date.now()),
};
