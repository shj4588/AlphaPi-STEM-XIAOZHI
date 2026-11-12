'use strict';
/**
 * local.js —— 本地曲库（原 r1-music / 8010 那半边，2026-10-17 并入 mini-lx）
 *
 * 合并前：r1-music 是一台独立的 Python(Flask) 容器（8010，mutagen 读标签），
 *         mini-lx 是 Node 容器（8100，在线音源），两个容器两个端口。
 * 合并后：一个容器、一个进程、一个端口 8100 ——
 *         /index.json 一次吐出「本地曲目 + 在线随机池 + 电台」，
 *         R1 的 files/music_index 只写一个地址就够了。
 *
 * ★ 标签读取用 music-metadata（npm，纯 JS，无需编译）取代 mutagen。
 *   它只读文件头部几 KB（我们连时长都不读），比扫整首快得多。
 *
 * ★★ 扫描结果落盘 files/local_cache.json：按 size+mtimeMs 命中就跳过解析。
 *   1188 首第一次全解析要点时间（并发 8 也要几十秒），之后只有新增/改动的
 *   才真正解析 —— 容器重建也丢不掉（files 是挂载卷）。
 */
const fs = require('fs');
const path = require('path');

const DIR = process.env.MUSIC_DIR || '/music';
const RADIOS_FILE = process.env.RADIOS_FILE || path.join(__dirname, 'files', 'radios.json');
// 用 LOCAL_CACHE 可以挪走（跑测试时不污染 files/ 里那份真缓存）
const CACHE_FILE = process.env.LOCAL_CACHE || path.join(__dirname, 'files', 'local_cache.json');
const EXTS = ['.mp3', '.m4a', '.flac', '.wav', '.ogg', '.aac', '.wma', '.ape'];
/** 列表缓存（只防连点，真正的省时靠落盘的标签 memo） */
const TTL = 10000;
/** 并发解析标签的线程数（IO 密集，8 个够） */
const CONC = 8;

let mm = null;
try { mm = require('music-metadata'); } catch { mm = null; }

const MIME = {
  '.mp3': 'audio/mpeg', '.m4a': 'audio/mp4', '.flac': 'audio/flac',
  '.wav': 'audio/wav', '.ogg': 'audio/ogg', '.aac': 'audio/aac',
  '.wma': 'audio/x-ms-wma', '.ape': 'audio/x-monkeys-audio',
};

/** '晴天 - 周杰伦' -> ['晴天','周杰伦'] */
function hasCjk(s) { return /[一-鿿]/.test(String(s || '')); }

function splitTitle(name) {
  // ⚠️ 裸 '-' 只在两侧都是中文时才拆 —— 否则 'Lose-Yourself' 会被误拆
  for (const sep of [' - ', '－']) {
    if (name.includes(sep)) {
      const i = name.indexOf(sep);
      const a = name.slice(0, i).trim(), b = name.slice(i + sep.length).trim();
      if (a && b) return [a, b];
    }
  }
  const i = name.indexOf('-');
  if (i > 0) {
    const a = name.slice(0, i).trim(), b = name.slice(i + 1).trim();
    if (a && b && hasCjk(a) && hasCjk(b)) return [a, b];
  }
  return [name, ''];
}

// ================= 标签乱码还原（2026-10-19） =================
// 现象：曲库里「周杰伦 / 青花瓷」这类老 MP3 显示成 ÖÜ½ÜÂ× / Çà»¨´É。
// 根因：**服务端**，不是设备也不是 player —— 老 MP3 的 ID3 标签正文其实是 GBK，
//   但 frame 的 encoding 字节写的是 0（= ISO-8859-1）。music-metadata 照规范
//   逐字节按 latin-1 解 ⇒ 出来一坨西欧重音字母。把字节取回、按 GBK 重解就对了。
// ★ 这段是从 Python 版 music-server（v1.4，2026-10-11）移植过来的 ——
//   2026-10-17 两个容器合并、本地曲库用 Node 重写时**漏了这一步**，于是乱码复发。

/** 宽判据：含任意 CJK（汉字 / 假名 / 韩文）⇒ 这字段已经是好的，别动。
 *  写成 \u 转义是故意的 —— 免得哪天文件被存成别的编码，正则跟着坏。 */
const RE_CJK = /[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uac00-\ud7af]/;
/** 窄判据：只要汉字（用来确认"按 GBK 解出来确实是中文"） */
const RE_HAN = /[\u4e00-\u9fff]/;

/**
 * windows-1252 把 0x80-0x9F 解成了这些码位（ISO-8859-1 那边是没有字符的）。
 * music-metadata 走的是严格 latin-1（1:1，见 @borewit/text-codec 的 decodeLatin1），
 * 所以正常路径用不到这张表 —— 但 WAV/ASF 的 INFO 走的是 windows-1252，
 * 兜着以便同样的逻辑也能修。
 */
const W1252_REV = new Map([
  [0x20ac, 0x80], [0x201a, 0x82], [0x0192, 0x83], [0x201e, 0x84],
  [0x2026, 0x85], [0x2020, 0x86], [0x2021, 0x87], [0x02c6, 0x88],
  [0x2030, 0x89], [0x0160, 0x8a], [0x2039, 0x8b], [0x0152, 0x8c],
  [0x017d, 0x8e], [0x2018, 0x91], [0x2019, 0x92], [0x201c, 0x93],
  [0x201d, 0x94], [0x2022, 0x95], [0x2013, 0x96], [0x2014, 0x97],
  [0x02dc, 0x98], [0x2122, 0x99], [0x0161, 0x9a], [0x203a, 0x9b],
  [0x0153, 0x9c], [0x017e, 0x9e], [0x0178, 0x9f],
]);

/** 字符串 -> 原始字节；只要有一个字符不在 latin-1/windows-1252 里就返回 null */
function toLatin1Bytes(s) {
  const out = [];
  for (const ch of s) {                     // for..of 按 code point 走，代理对不会拆坏
    const cp = ch.codePointAt(0);
    if (cp <= 0xff) { out.push(cp); continue; }
    const b = W1252_REV.get(cp);
    if (b === undefined) return null;
    out.push(b);
  }
  return Buffer.from(out);
}

/**
 * 按 GBK 规则扫一遍字节，返回 (成对双字节数, 孤立高字节数)。
 *
 * ★★ 尾字节只认 >= 0x80（GB2312 区的汉字尾字节是 0xA1-0xFE），**不认 0x40-0x7E**。
 *   原因是误伤实测：'Mötley Crüe' 里 ö+t(0x74)、ü+e(0x65) 按 GBK 规范算合法双字节
 *   （尾字节范围确实是 0x40-0xFE），于是 pairs=2 / stray=0 直接骗过判据 ⇒ 变成
 *   'M鰐ley Cr黣'。'Jökulsárlón' 同理。收紧到 >= 0x80 后：
 *     西文重音字母后面跟的一定是 ASCII 字母 ⇒ 全部落进 stray ⇒ 拒绝；
 *     真中文（GB2312 常用字）尾字节 >= 0xA1 ⇒ 照常命中。
 *   代价只有：GBK 扩展区里尾字节落在 0x40-0x7E 的极生僻字识别不出来，无所谓。
 */
function gbkPairs(b) {
  let i = 0, pairs = 0, stray = 0;
  while (i < b.length) {
    const c = b[i];
    if (c >= 0x81) {                        // GBK 首字节 0x81-0xFE
      const nxt = i + 1 < b.length ? b[i + 1] : -1;
      if (nxt >= 0x80 && nxt <= 0xfe) { pairs++; i += 2; continue; }
      stray++; i++; continue;
    }
    i++;
  }
  return { pairs, stray };
}

/** GBK / Big5 解码器（fatal：非法字节直接抛，好过吞成 U+FFFD 再误判） */
const DEC = {};
function dbcsDec(enc) {
  if (DEC[enc] !== undefined) return DEC[enc];
  let d = null;
  try { d = new TextDecoder(enc, { fatal: true }); } catch { d = null; }
  if (!d) {
    // 兜底：万一镜像是 small-icu（官方镜像默认 full-icu，一般不会走到这）
    try { d = { decode: b => require('iconv-lite').decode(b, enc) }; } catch { d = null; }
  }
  DEC[enc] = d;
  return d;
}
function decodeDbcs(buf, enc) {
  const d = dbcsDec(enc);
  if (!d) return null;
  try { return d.decode(buf); } catch { return null; }   // 非法字节
}
/** 给 /api/health 用的自检：GBK 解码器在不在 */
function gbkSupported() { return !!dbcsDec('gbk'); }

/**
 * 把「GBK 字节被当成 latin-1 解出来」的乱码还原成中文。
 *
 * ⚠️ 误伤防线（关键）：正常的西欧重音字母也能凑出合法 GBK 双字节 ——
 *     Tiësto(ë+s→雜) / Monáe(á+e→醗) / Angèle(è+l→鑜) / Hawái(á+i→醝)。
 *   区别在于真乱码是**一整片连续的双字节对**（中文词至少 2 字），
 *   重音字母是**孤立的单字节**。所以要求：
 *     · 按 GBK 对齐扫描后没有孤立高字节（stray == 0）
 *     · 至少 2 对；只有 1 对时整个字段必须全是高字节（如 'µ±' → '当'）
 *     · 解出来确实含汉字
 */
function fixMojibake(s) {
  if (!s) return s;
  if (RE_CJK.test(s)) return s;                     // 已经是中文，别动
  const b = toLatin1Bytes(s);
  if (!b || b.length < 2) return s;
  const { pairs, stray } = gbkPairs(b);
  if (stray || pairs < 1) return s;
  let allHigh = true;
  for (const x of b) if (x < 0x80) { allHigh = false; break; }
  if (pairs < 2) {
    // 只有一对时风险最大：'öü'(F6 FC) 这种连续两个重音字母，同样满足
    // "全高字节 + 恰好一对" ⇒ 会被解成 '鳇'。再卡一道 GB2312 汉字区
    // （首字节 0xB0-0xD7、尾字节 0xA1-0xFE）：'µ±'(B5B1 → 当) 进得来，'öü' 被挡掉。
    const inHanZone = b.length === 2 && b[0] >= 0xb0 && b[0] <= 0xd7 && b[1] >= 0xa1 && b[1] <= 0xfe;
    if (!(allHigh && inHanZone)) return s;          // 单个重音字母，不是乱码
  }
  for (const enc of ['gbk', 'big5']) {
    const out = decodeDbcs(b, enc);
    if (out && RE_HAN.test(out)) return out;
  }
  return s;
}

/**
 * 标签值明显是坏的（空 / 只有问号 / 替换符）—— 这种不是编码问题，是标签本身写坏
 * （例：'陈奕迅 - 浮夸.mp3' 里 title='??' artist='???'），只能回退文件名。
 */
function looksBroken(s) {
  if (!s) return true;
  if (s.includes('\ufffd')) return true;
  return !s.trim().replace(/^[?？]+/, '').replace(/[?？]+$/, '').trim();
}

// ---------------- 标签 memo ----------------
let MEMO = null;
let memoDirty = false;
/** 缓存格式版本。v1 = 没修过乱码的老缓存；改成 2 让老缓存里的乱码**就地修一遍**
 *  （不重新读文件 —— 缓存里就有原始字符串，直接过 fixMojibake 即可，秒生效）。 */
const CACHE_V = 2;
/** 本次进程修掉了几条缓存里的乱码（/api/health 看得见） */
let MOJI_FIXED = 0;

function loadMemo() {
  if (MEMO) return MEMO;
  MEMO = {};
  try {
    const j = JSON.parse(fs.readFileSync(CACHE_FILE, 'utf8'));
    for (const t of (j.tracks || [])) {
      if (!t || !t.file) continue;
      if (t.v !== CACHE_V) {
        // 老缓存：title/artist/album 里可能存着 'Çà»¨´É' 这种串，就地还原
        const a = fixMojibake(t.title || ''), b = fixMojibake(t.artist || ''), c = fixMojibake(t.album || '');
        if (a !== (t.title || '') || b !== (t.artist || '') || c !== (t.album || '')) MOJI_FIXED++;
        t.title = a; t.artist = b; t.album = c; t.v = CACHE_V;
        memoDirty = true;
      }
      MEMO[t.file] = t;
    }
  } catch { /* 没有就从空开始 */ }
  return MEMO;
}

function saveMemo() {
  try {
    fs.mkdirSync(path.dirname(CACHE_FILE), { recursive: true });
    fs.writeFileSync(CACHE_FILE, JSON.stringify({ tracks: Object.values(MEMO) }));
    memoDirty = false;
  } catch { /* 只读环境也能跑，只是重启后要重解析一遍 */ }
}

/** 拿一个文件的标签：memo 命中直接用，否则解析后写回 */
async function tagsFor(abs, rel) {
  let size = 0, mt = 0;
  try {
    const st = fs.statSync(abs);
    size = st.size; mt = Math.floor(st.mtimeMs);
  } catch { return { title: '', artist: '', album: '' }; }
  const m = loadMemo()[rel];
  if (m && m.size === size && m.mtimeMs === mt) {
    return { title: m.title || '', artist: m.artist || '', album: m.album || '' };
  }
  let t = { title: '', artist: '', album: '' };
  if (mm) {
    try {
      const md = await mm.parseFile(abs, {
        duration: false, skipCovers: true, skipPostHeaders: true,
      });
      const c = (md && md.common) || {};
      t = {
        title: fixMojibake(c.title || ''),
        artist: fixMojibake(c.artist || c.albumartist || ''),
        album: fixMojibake(c.album || ''),
      };
    } catch { /* 读不到就交给文件名解析 */ }
  }
  MEMO[rel] = {
    file: rel, size, mtimeMs: mt, v: CACHE_V,
    title: t.title, artist: t.artist, album: t.album,
  };
  memoDirty = true;
  return t;
}

// ---------------- 扫描 ----------------
function walk(dir, base, out) {
  let ents;
  try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
  const dirs = [], files = [];
  for (const e of ents) {
    if (e.isDirectory()) dirs.push(e.name);
    else if (e.isFile() && EXTS.includes(path.extname(e.name).toLowerCase())) files.push(e.name);
  }
  files.sort();
  dirs.sort();
  for (const f of files) {
    out.push(path.relative(base, path.join(dir, f)).replace(/\\/g, '/'));
  }
  for (const d of dirs) walk(path.join(dir, d), base, out);
}

let CACHED = { ts: 0, list: null, radios: null };

function readRadios() {
  try {
    const j = JSON.parse(fs.readFileSync(RADIOS_FILE, 'utf8'));
    const arr = Array.isArray(j) ? j : (j.radios || []);
    // 字段名各家不同（r1-music 用 url，别的清单可能写 stream/addr/src），都认
    const u = r => String(r.url || r.stream || r.addr || r.src || r.link || '');
    return arr.filter(r => r && r.name && u(r))
      .map(r => ({ name: String(r.name), url: u(r) }));
  } catch { return []; }
}

/**
 * 列出本地曲目（按路径排序，和 Python 版一致）。
 * @returns {Promise<Array<{file:string,title:string,artist:string,album:string}>>}
 */
async function list() {
  const now = Date.now();
  if (CACHED.list && now - CACHED.ts < TTL) return CACHED.list;

  const files = [];
  walk(DIR, DIR, files);

  // 并发解析标签，但**结果按 files 原顺序**回填（顺序 = 曲库序号，不能乱）
  const tags = new Array(files.length);
  for (let i = 0; i < files.length; i += CONC) {
    const chunk = files.slice(i, i + CONC);
    const rs = await Promise.all(chunk.map(rel => tagsFor(path.join(DIR, rel), rel)));
    for (let k = 0; k < rs.length; k++) tags[i + k] = rs[k];
  }
  if (memoDirty) saveMemo();

  const out = files.map((rel, i) => {
    const parts = rel.split('/');
    const stem = path.basename(rel, path.extname(rel));
    // 1) 文件名 / 目录名解析（兜底）
    let fTitle = stem, fArtist = '';
    const [a, b] = splitTitle(stem);
    if (b) { fTitle = a; fArtist = b; }
    else if (parts.length >= 2) { fArtist = parts[parts.length - 2]; }
    // 2) 内嵌标签优先（但 '??' 这种写坏的标签不算，回退文件名）
    const tg = tags[i] || {};
    return {
      file: rel,
      title: looksBroken(tg.title) ? fTitle : tg.title,
      artist: looksBroken(tg.artist) ? fArtist : tg.artist,
      album: looksBroken(tg.album) ? '' : tg.album,
    };
  });

  CACHED = { ts: now, list: out, radios: readRadios() };
  return out;
}

/** 电台清单（与 list() 共用缓存，避免每请求都读盘） */
function radios() {
  if (CACHED.radios) return CACHED.radios;
  return readRadios();
}

/** 强制重扫（传完歌不想等 TTL 就调 /reindex） */
function reindex() {
  CACHED = { ts: 0, list: null, radios: null };
}

/**
 * 写回电台清单（Web 上「加台 / 删台」用）。
 * 只留 name + 能认出来的 http(s) 地址，其余字段一律丢弃 —— 免得存进去脏数据。
 * @param {Array<{name:string,url:string}>} list
 * @returns {{ok:boolean,count:number,err?:string}}
 */
function saveRadios(list) {
  const urlOf = r => String((r && (r.url || r.stream || r.addr || r.src || r.link)) || '').trim();
  const arr = (Array.isArray(list) ? list : [])
    .map(r => ({ name: String((r && r.name) || '').trim(), url: urlOf(r) }))
    .filter(r => r.name && /^https?:\/\//i.test(r.url));
  try {
    fs.mkdirSync(path.dirname(RADIOS_FILE), { recursive: true });
    fs.writeFileSync(RADIOS_FILE, JSON.stringify(arr, null, 2), 'utf8');
    // 只换 radios，别把已扫好的本地列表缓存清掉（否则加个台要重扫几千个文件）
    CACHED = { ts: CACHED.ts, list: CACHED.list, radios: arr };
    return { ok: true, count: arr.length };
  } catch (e) {
    return { ok: false, count: 0, err: String((e && e.message) || e) };
  }
}

/**
 * 相对路径 -> 绝对路径，并挡住 ../ 穿越。
 *
 * ⚠️ 两边都要 resolve：DIR 可能是 'C:/a/b'（compose 里写的）也可能是 '/music'，
 * 而与 path.sep（Windows 是 '\'）直接拼会永远不相等 —— 实测在 Windows 上
 * 会因此把所有文件都判成「不在目录内」而 404。
 */
function absOf(rel) {
  const base = path.resolve(DIR);
  const abs = path.resolve(base, rel);
  if (abs !== base && !abs.startsWith(base + path.sep)) return null;
  return abs;
}

function mimeOf(rel) {
  return MIME[path.extname(rel).toLowerCase()] || 'audio/mpeg';
}

/**
 * 自检：本地库为什么是 0 首？（挂载没生效 / 目录空的 / 扩展名不认 / 权限）
 * /api/health 的 local 字段直接用它 —— 不用进容器就能看出来。
 */
function diag() {
  const d = {
    dir: DIR, exists: false, isDir: false,
    topEntries: [], scanned: 0, sample: [], hasTags: !!mm, err: null,
    // 中文标签乱码修复自检：gbk = 解码器可用；mojibakeFixed = 本次修了几条缓存
    gbk: gbkSupported(), mojibakeFixed: MOJI_FIXED, cacheV: CACHE_V,
  };
  try {
    const st = fs.statSync(DIR);
    d.exists = true;
    d.isDir = st.isDirectory();
    if (d.isDir) d.topEntries = fs.readdirSync(DIR).slice(0, 10);
  } catch (e) {
    d.err = String(e && e.message || e);   // 多半是 ENOENT（没挂上）
    return d;
  }
  try {
    const files = [];
    walk(DIR, DIR, files);
    d.scanned = files.length;
    d.sample = files.slice(0, 5);
  } catch (e) {
    d.err = String(e && e.message || e);   // 多半是 EACCES（权限）
  }
  return d;
}

module.exports = {
  list, radios, saveRadios, reindex, absOf, mimeOf, diag, DIR, RADIOS_FILE, hasTags: !!mm,
  fixMojibake, looksBroken, gbkSupported,
};
