'use strict';
/**
 * ablocal.js —— NAS 本地有声书库（2026-11-04）
 *
 * 与在线书库（落雪 /api/ab/*，靠音乐平台搜书名+集）并列的第二路来源：
 * 直接扫 /music 挂载目录里的音频文件，按**文件名**归档成「书 → 集」，
 * 播放走已有的 /m/<rel> 直链（Range 支持、走内网，比在线取链快一个量级）。
 *
 * ★ 为什么要单独建索引而不是复用 local.js：
 *   local.js 是「歌曲」模型（title/artist/album 来自内嵌标签），有声书的层级是
 *   「书 / 合集-子书 / 集」，且集数、书名只能从文件名里认，标签里根本没有。
 *   两边用途不同，共用一个模型只会互相污染（歌曲列表里混进 2000 集评书）。
 *
 * ★ 命名格式（2026-11-04 用户提供的四种真实样本，全部要认）：
 *   A 道诡异仙-第0集-引子.m4a                 书名-第N集-小标题
 *   B 凡人修仙传 第0集 预告.m4a                书名 第N集 小标题（空格/全角空格）
 *   C 1.放开那个女巫-我就是四王子-ZmAudio.m4a   前置序号=集数，书名-小标题-水印
 *   D 三体139-【黑暗森林Ⅱ】第85集.mp3          合集：总序号139 + 【子书】 + 子书内第85集
 *
 *   D 这种「一本合集装三部曲」是本模块最麻烦的地方：合集要能显示总集数，
 *   点开要落到三部子书，子书各自有自己的集数体系与播放记忆。
 *   ⇒ 数据模型固定两层：collection(合集) → sub(子书) → episodes(集)。
 *
 * ★ 缓存：扫盘只读目录名（不读标签），几千个文件只是几毫秒；仍然落盘
 *   files/ab_local.json（挂载卷），容器重启后第一屏 instantly 有数据，
 *   随后后台再对齐一次盘上现状（用户原话：源信息可以预先就存在 docker 上缓存好）。
 */
const fs = require('fs');
const path = require('path');

/**
 * ★ 2026-11-04 定稿：本地有声书库是**独立目录**（默认 /audiobook，单独挂载），
 *   里面只放有声书 ⇒ 不再需要「像不像书」的启发式去跟歌曲区分，凡是音频都收。
 *   （旧方案是扫 /music 整个曲库再猜，靠「≥2 个文件带集数」过滤歌曲，误判多。）
 *   想扫别的目录就设 ABL_DIR（compose 里加一行 - ABL_DIR=/volume1/audiobook）。
 */
const DIR = process.env.ABL_DIR || '/audiobook';
const CACHE_FILE = process.env.ABL_CACHE || path.join(__dirname, 'files', 'ab_local.json');
const POS_FILE = process.env.ABL_POS || path.join(__dirname, 'files', 'ab_local_pos.json');

const EXTS = /\.(mp3|m4a|m4b|aac|flac|wav|ogg|wma|ape)$/i;

/**
 * ★ 2026-11-04 晚：有声书目录常自带 sidecar（用户给的样本「道诡异仙」）——
 *   cover.jpeg(287KB) / cover_small.jpg(6KB) / desc.txt / reader.txt / metadata.json(190KB)
 *
 *   metadata.json 是 Audiobookshelf 那套结构，实测字段：
 *     title / subtitle / authors[] / narrators[] / series[] / genres[] /
 *     publishedYear / publisher / description / chapters[{id,start,end,title}]
 *   chapters 的 start/end 是**整本累计秒**（因为本来是给合并 m4b 用的），
 *   但对「一本一个文件」的目录同样有用：end-start 就是这一集的时长，
 *   而时长是文件名里根本没有的信息。
 *
 *   四样都收，但都不强求（三体 / 庆余年 就没有）：
 *     ① cover       列表缩略图。优先 cover_small（6KB，R1 内存/带宽都紧）
 *     ② author/narrator/year  书名底下那行，比从长目录名里猜靠谱
 *     ③ desc        简介，列表只显示一小段，详情页可展开
 *     ④ dur         每集时长（秒），列表里显示成 23:45
 *   没有 sidecar 时字段留空，前端全都要能兜底。
 */
const SIDECAR_COVER = /^(cover_small|cover|folder|poster|front)\.(jpe?g|png|webp)$/i;
const SIDECAR_DESC = /^(desc|description|intro|简介)\.txt$/i;
const SIDECAR_READER = /^reader\.txt$/i;
const SIDECAR_META = /^metadata\.json$/i;
const COVER_ORDER = { cover_small: 0, cover: 1, folder: 2, poster: 3, front: 4 };
/** 目录黑名单：群晖缩略图 / 回收站 / 隐藏目录，进不去 */
const SKIP_DIRS = /^(#recycle|@eaDir|\$RECYCLE\.BIN|\.|node_modules|System Volume Information)$/i;
const TTL = 10000;              // 扫盘结果记忆 10s（防连点）
const POS_MAX = 500;            // 进度表上限（按书写，不会失控）

/** 内存态：当前书库 + 进度表 */
let MEM = { at: 0, dir: DIR, books: [] };
let scanAt = 0;
let POS = {};

// ---------------------------------------------------------------- 命名解析

/** 全角转半角 + 去水印尾缀（ZmAudio / 更多资源 之类） */
function normName(s) {
  return String(s || '')
    .replace(/[\u3000\u00a0]/g, ' ')
    .replace(/[-_ ]?(?:ZmAudio|ZMAudio|zmAudio)$/, '')
    // ★ 必须带「实体内容」才剥：这条正则以前 (?:...)? 整体可选，等于把文件名末尾的
    //   任意 `）` 都吃掉了 —— 「第90集（完）」会剩个 `（完`。
    .replace(/[-_ (（]*(?:更多资源|更多章节|公众号[:：]?\S*|关注公众号\S*|微信\S*)[)）]*$/, '')
    .trim();
}

/**
 * 从文件名解析出归属。
 * @returns {{book:string, sub:string, idx:number, ep:number, title:string, ok:boolean}}
 *          book = 书名（合集名）；sub = 子书（仅 D 型有）；idx = 总序号；ep = 集数（-1 未知）
 */
function parseName(base, dirName) {
  let name = normName(base.replace(EXTS, ''));
  let idx = 0;          // 前置序号或书名尾部的总序号
  let sub = '';
  let ep = -1;
  let title = '';

  // ① 前置序号：`1.` / `01、` / `001_` / `12 `
  const mLead = name.match(/^(\d{1,4})\s*[.、．)_\-]\s*/);
  if (mLead) { idx = parseInt(mLead[1], 10); name = name.slice(mLead[0].length).trim(); }
  // ①b 纯数字文件名（目录=书名、文件=001.mp3 这种最省事的整理方式）
  let pure = false;
  if (!name || /^\d{1,4}$/.test(name)) {
    if (/^\d{1,4}$/.test(name)) { idx = parseInt(name, 10); pure = true; }
    name = '';
  }

  // ② 子书标记：优先 【】，其次 []／［］
  //   ★ 判据有两条，缺一就会误伤（2026-11-04 实测踩过）：
  //     ① 括号里本身是「第N集」不算子书；
  //     ② **括号必须在集号之前**。庆余年写成 `… 第088集 大婚【上】`，
  //        括号在后面、内容是小标题的上下半，按老规则会被拆成「上」「下」两本子书，
  //        一本书被劈成 259 + 493 两半。三体 `三体139-【黑暗森林Ⅱ】第85集` 才在集号前。
  const epProbe = name.match(/第\s*\d{1,4}\s*[集回话章]/);
  const beforeEp = i => (!epProbe || i < epProbe.index);
  const mSub = name.match(/【([^】]{1,40})】/);
  if (mSub && beforeEp(mSub.index)) {
    sub = mSub[1].trim();
    name = (name.slice(0, mSub.index) + ' ' + name.slice(mSub.index + mSub[0].length)).trim();
  } else {
    const mSub2 = name.match(/[\[［]([^\]］]{1,40})[\]］]/);
    if (mSub2 && beforeEp(mSub2.index) && !/第\s*\d+\s*[集回话章]/.test(mSub2[1])) {
      sub = mSub2[1].trim();
      name = (name.slice(0, mSub2.index) + ' ' + name.slice(mSub2.index + mSub2[0].length)).trim();
    }
  }

  // ③ 集数：第N集/回/话/章（可能出现在子书之后），余下当小标题
  const mEp = name.match(/第\s*(\d{1,4})\s*[集回话章]\s*[.、,，:：\-—_·]*\s*(.*)$/);
  if (mEp) {
    ep = parseInt(mEp[1], 10);
    title = String(mEp[2] || '').replace(/^[-—_·.、,，:：\s]+/, '').replace(/[（(](?:完|完结|大结局)[)）]/g, '').trim();
    name = name.slice(0, mEp.index);
  } else {
    // ③b 漏了「集」字的（道诡异仙-第53-追来的佛陀.m4a）——
    //    不认的话它会另立一本「道诡异仙-第53-追来的佛陀」单集书，
    //    还把整本的章节数对不上、连累 1300 集全没时长。
    const mEp2 = name.match(/第\s*(\d{1,4})\s*[-—_·:：]\s*(.+)$/);
    if (mEp2) {
      ep = parseInt(mEp2[1], 10);
      title = String(mEp2[2] || '').trim();
      name = name.slice(0, mEp2.index);
    }
  }

  // ④ 书名 = 剩下那段；剥掉尾部总序号（三体139）与分隔符
  let book = name.replace(/[\s\-—_·.、,，:：]+$/g, '');
  const mTail = book.match(/^(.*?)[\s\-—_]*(\d{1,4})$/);
  if (mTail && mTail[1].trim()) { book = mTail[1]; if (!idx) idx = parseInt(mTail[2], 10); }
  book = book.replace(/[\s\-—_·.、,，:：]+$/g, '').trim();

  // ⑤ 兜底：文件名里没有书名（如目录=书名、文件=001.mp3）⇒ 用父目录名
  if (!book) book = String(dirName || '').trim();

  // ⑥ 没有「第N集」但有前置序号（C 型）⇒ 序号即集数，剩下一段拆「书名-小标题」
  //   纯数字文件名（001.mp3）连 0 都算合法集号，所以单独判 pure
  if (ep < 0 && (idx > 0 || pure)) {
    ep = idx;
    const mSep = book.match(/^(.+?)\s*[-－—]\s*(.+)$/);
    if (mSep) { book = mSep[1].trim(); if (!title) title = mSep[2].trim(); }
  }
  // ⑦ 书名里还挂着「-小标题」（A 型剥掉集数后只剩书名，正常；这里只处理残留）
  if (ep >= 0 && !title) {
    const mSep2 = book.match(/^(.+?)\s*[-－—]\s*(.+)$/);
    if (mSep2) { /* 书名里带连字符的多为系列名，保留整段，不拆 */ }
  }

  return { book, sub, idx, ep, title, ok: !!book };
}

// ---------------------------------------------------------------- 侧车元数据

function readText(abs, max) {
  try {
    let s = fs.readFileSync(abs).toString('utf8');
    if (s.charCodeAt(0) === 0xfeff) s = s.slice(1);
    if (max && s.length > max) s = s.slice(0, max);
    return s;
  } catch { return ''; }
}

/** authors 可能是 ['狐尾的笔\\传说中的方片k'] 也可能是 ['忘语 / 光合积木'] ⇒ 统一拆成「 / 」 */
function listJoin(v, keep) {
  const arr = Array.isArray(v) ? v : (v ? [v] : []);
  const out = [];
  for (const x of arr) {
    for (const p of String(x).split(/[\\/、,;，；|]+/)) {
      const t = p.trim();
      if (t && out.indexOf(t) < 0) out.push(t);
    }
  }
  return out.slice(0, keep || 3).join(' / ');
}

/** 简介清洗：去掉「内容简介」小标题、压缩空行（desc.txt 与 metadata.description 是同一份） */
function cleanDesc(s) {
  return String(s || '')
    .replace(/^\s*(内容简介|简介|文案|剧情简介)\s*[:：]?\s*/, '')
    .replace(/\r\n?/g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

/** 章节时长（秒）：chapters[i] 的 end-start；脏数据一律算 0 */
function chapDur(ch) {
  const a = Number(ch && ch.start), b = Number(ch && ch.end);
  if (!isFinite(a) || !isFinite(b) || b <= a) return 0;
  return Math.round(b - a);
}

/**
 * 读一个书目录的 sidecar。
 * @param {string} dirAbs  绝对目录
 * @param {string} relDir  相对 ABL_DIR 的目录（给 URL 用，根目录传 ''）
 */
function readSide(dirAbs, relDir) {
  const side = { cover: '', desc: '', descAll: '', reader: '', author: '', narrator: '',
                 title: '', year: '', publisher: '', chapters: null };
  let ents = [];
  try { ents = fs.readdirSync(dirAbs); } catch { return side; }

  // ① 封面：cover_small 优先（R1 上 6KB vs 287KB 的差别是实打实的）
  const cov = ents.filter(n => SIDECAR_COVER.test(n)).sort((a, b) => {
    const ra = COVER_ORDER[a.replace(/\.[^.]+$/, '').toLowerCase()] ?? 9;
    const rb = COVER_ORDER[b.replace(/\.[^.]+$/, '').toLowerCase()] ?? 9;
    return ra - rb;
  });
  if (cov.length) side.cover = (relDir ? relDir + '/' : '') + cov[0];

  // ② reader.txt = 演播（一行）；desc.txt = 简介
  const rd = ents.find(n => SIDECAR_READER.test(n));
  if (rd) side.reader = (readText(path.join(dirAbs, rd), 400).split(/[\r\n]+/)[0] || '').trim();
  const ds = ents.find(n => SIDECAR_DESC.test(n));
  if (ds) side.descAll = readText(path.join(dirAbs, ds), 6000);

  // ③ metadata.json（坏了就当没有，不能因为一本书的 json 坏掉整库扫不出来）
  const mj = ents.find(n => SIDECAR_META.test(n));
  if (mj) {
    try {
      const j = JSON.parse(readText(path.join(dirAbs, mj), 4 * 1024 * 1024) || '{}');
      side.title = String(j.title || '').trim();
      side.author = listJoin(j.authors, 2);
      side.narrator = listJoin(j.narrators, 2);
      side.year = String(j.publishedYear || (j.publishedDate || '').slice(0, 4) || '');
      side.publisher = String(j.publisher || '').trim();
      if (!String(side.descAll || '').trim()) side.descAll = String(j.description || '');
      if (Array.isArray(j.chapters) && j.chapters.length) {
        side.chapters = j.chapters.filter(c => c && (c.title != null || c.end != null));
      }
    } catch { /* 坏 json */ }
  }
  if (!side.narrator) side.narrator = side.reader;
  side.desc = cleanDesc(side.descAll);
  side.descAll = side.desc;
  return side;
}

// ---------------------------------------------------------------- 扫盘

function walk(dir, rel, out, depth) {
  if (depth > 8) return;
  let ents;
  try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of ents) {
    const name = e.name;
    if (e.isDirectory()) {
      if (SKIP_DIRS.test(name)) continue;
      walk(path.join(dir, name), rel ? rel + '/' + name : name, out, depth + 1);
      continue;
    }
    if (!EXTS.test(name)) continue;
    let st;
    try { st = fs.statSync(path.join(dir, name)); } catch { continue; }
    if (!st.isFile() || st.size < 200 * 1024) continue;   // 试听/片段（<200KB）不收
    out.push({ rel: rel ? rel + '/' + name : name, name, size: st.size, mtime: st.mtimeMs });
  }
}

/** id 编解码：书 = b64(book\x01sub)，合集 = b64(book\x02) */
const b64 = s => Buffer.from(String(s), 'utf8').toString('base64url');
const unb64 = s => { try { return Buffer.from(String(s), 'base64url').toString('utf8'); } catch { return ''; } };
const idOf = (book, sub) => b64(sub ? book + '\u0001' + sub : book + '\u0001');
const collIdOf = book => b64(book + '\u0002');

/** 解析一个 id */
function parseId(id) {
  const s = unb64(id);
  if (!s) return null;
  if (s.endsWith('\u0002')) return { book: s.slice(0, -1), sub: null, coll: true };
  const i = s.indexOf('\u0001');
  if (i < 0) return null;
  return { book: s.slice(0, i), sub: s.slice(i + 1), coll: false };
}

/** rel 的目录部分（'a/b/c.mp3' → 'a/b'；根目录 → ''） */
function dirOfRel(rel) {
  const s = String(rel || '');
  const i = s.lastIndexOf('/');
  return i < 0 ? '' : s.slice(0, i);
}

/** 一本书的展示信息（sidecar 有就全用，没有就全空 —— 前端兜底到 name） */
function bookMeta(g) {
  const s = (g && g.side) || {};
  const desc = String(s.desc || '');
  return {
    title: s.title || '', author: s.author || '', narrator: s.narrator || '',
    year: s.year || '',
    cover: s.cover ? '/m/' + encPath(s.cover) : '',
    intro: desc.replace(/\s+/g, ' ').slice(0, 160),
    desc: desc.slice(0, 2000),
    descLen: desc.length,
  };
}

/** 一本/一卷的总时长（秒）；没有章节时长就是 0 */
function sumDur(files) {
  let n = 0;
  for (const f of files || []) n += (f.dur || 0);
  return n;
}

/**
 * 全量扫盘建树。
 * 输出：books[] = 普通书（有 episodes）或合集（有 subs[]），两者都带 count。
 */
function scan() {
  const files = [];
  walk(DIR, '', files, 0);

  /** key = book\x01sub → 组 */
  const groups = new Map();
  for (const f of files) {
    const dirName = f.rel.includes('/') ? f.rel.split('/').slice(-2, -1)[0] : '';
    const p = parseName(f.name, dirName);
    if (!p.ok) continue;
    // 既没有集数、也没有子书、也没进「疑似书目录」⇒ 当普通歌曲跳过（避免把歌单当书）
    const key = p.book + '\u0001' + p.sub;
    let g = groups.get(key);
    if (!g) { g = { book: p.book, sub: p.sub, files: [], idxMin: 1e9, depth: f.rel.split('/').length, explicit: 0 }; groups.set(key, g); }
    // ★ explicit = 文件名里**明确**给了集数（第N集 / 前置序号 / 尾部总序号）。
    //   判「这是不是一本书」靠它，否则 '晴天 - 周杰伦.mp3' 会被凑成「1 集的书」。
    const explicit = p.ep >= 0 || p.idx > 0;
    if (explicit) g.explicit++;
    g.files.push({ ep: p.ep, idx: p.idx, title: p.title, rel: f.rel, size: f.size, mtime: f.mtime, explicit });
    if (p.idx && p.idx < g.idxMin) g.idxMin = p.idx;
    g.depth = Math.min(g.depth, f.rel.split('/').length);
  }

  // 组内排序：ep 优先，缺 ep 的按文件名自然序；ep 重复时用 size/mtime 兜底稳定
  const sideCache = new Map();          // relDir → side（合集里几部子书同目录，别读三遍）
  for (const g of groups.values()) {
    g.files.sort((a, b) => {
      const ae = a.ep >= 0 ? a.ep : 1e9, be = b.ep >= 0 ? b.ep : 1e9;
      if (ae !== be) return ae - be;
      return String(a.rel).localeCompare(String(b.rel), 'zh-Hans-CN', { numeric: true });
    });
    // 补集数：缺 ep 的按顺序编号（从 1 开始，且不与已有 ep 冲突）
    let next = 1;
    const used = new Set(g.files.map(f => f.ep).filter(n => n >= 0));
    for (const f of g.files) {
      if (f.ep >= 0) continue;
      while (used.has(next)) next++;
      f.ep = next; used.add(next);
    }
    g.files.sort((a, b) => a.ep - b.ep);

    // ---- sidecar（封面/作者/演播/简介/章节时长）----
    const relDir = dirOfRel(g.files[0] && g.files[0].rel);
    if (!sideCache.has(relDir)) sideCache.set(relDir, readSide(path.join(DIR, relDir), relDir));
    g.side = sideCache.get(relDir);

    // ★ 章节时长对集：chapters 与文件数一致时按顺序一一对应（道诡异仙 1300↔1300 实测吻合）；
    //   对不上就整本不要时长 —— 宁可没有，也别把时长错配到别的集上。
    const chs = g.side && g.side.chapters;
    if (chs && chs.length === g.files.length) {
      for (let i = 0; i < g.files.length; i++) g.files[i].dur = chapDur(chs[i]);
    }
  }

  /** book → {book, subs:Map, plain:group|null} */
  const bookMap = new Map();
  for (const g of groups.values()) {
    // ★ 独立有声书目录：只要有文件就算一本书（不再要求「≥2 集 / 在书目录里」）。
    //   保留了 sub（合集）与 explicit（明确集数）两个字段，只用于排序与集号补全。
    const looksBook = !!g.book;
    if (!looksBook) continue;
    let b = bookMap.get(g.book);
    if (!b) { b = { book: g.book, subs: [], plain: null }; bookMap.set(g.book, b); }
    if (g.sub) b.subs.push(g); else b.plain = g;
  }

  const books = [];
  for (const b of bookMap.values()) {
    if (b.subs.length) {
      // 合集：子书按「组内最早的总序号」排（三体 地球往事 < 黑暗森林 < 死神永生）
      b.subs.sort((x, y) => (x.idxMin - y.idxMin) || x.sub.localeCompare(y.sub, 'zh-Hans-CN', { numeric: true }));
      const subs = b.subs.map(g => {
        const mt = bookMeta(g);
        return {
          id: idOf(b.book, g.sub), name: g.sub, sub: g.sub,
          count: g.files.length, title: mt.title, author: mt.author, narrator: mt.narrator,
          year: mt.year, cover: mt.cover, intro: mt.intro, desc: mt.desc, durSec: sumDur(g.files),
          episodes: g.files,
        };
      });
      const head = bookMeta(b.subs[0]);
      books.push({
        id: collIdOf(b.book), name: b.book, kind: 'coll',
        count: subs.reduce((n, s) => n + s.count, 0),
        title: head.title, author: head.author, narrator: head.narrator,
        year: head.year, cover: head.cover, intro: head.intro, desc: head.desc,
        durSec: subs.reduce((n, s) => n + s.durSec, 0),
        subs,
      });
    }
    if (b.plain) {
      const g = b.plain, mt = bookMeta(g);
      books.push({
        id: idOf(b.book, ''), name: b.book, kind: 'book',
        count: g.files.length, title: mt.title, author: mt.author, narrator: mt.narrator,
        year: mt.year, cover: mt.cover, intro: mt.intro, desc: mt.desc,
        durSec: sumDur(g.files), episodes: g.files,
      });
    }
  }
  books.sort((x, y) => String(x.name).localeCompare(String(y.name), 'zh-Hans-CN', { numeric: true }));

  MEM = { at: Date.now(), dir: DIR, books };
  scanAt = Date.now();
  persist();
  return MEM;
}

/** 确保数据新鲜（10s 内复用；fresh=1 强制重扫） */
function ensure(fresh) {
  if (fresh || !MEM.books.length || Date.now() - scanAt > TTL) scan();
  return MEM;
}

// ---------------------------------------------------------------- 落盘

function persist() {
  try {
    fs.mkdirSync(path.dirname(CACHE_FILE), { recursive: true });
    // 只存「书名 + 集骨架」，不存 size/mtime（盘上一有增减就整份重扫，够快）
    // ★ v2：带上 sidecar 那几项（封面/作者/演播/年份/简介/时长），
    //   这样容器重启后第一屏就有封面和作者，不用等第一次重扫。
    const ep = e => ({ ep: e.ep, title: e.title, rel: e.rel, dur: e.dur || 0 });
    const meta = b => ({ title: b.title || '', author: b.author || '', narrator: b.narrator || '',
      year: b.year || '', cover: b.cover || '', intro: b.intro || '',
      desc: (b.desc || '').slice(0, 1200), durSec: b.durSec || 0 });
    const slim = {
      v: 2, at: MEM.at, dir: MEM.dir,
      books: MEM.books.map(b => Object.assign(
        { id: b.id, name: b.name, kind: b.kind, count: b.count }, meta(b), {
          subs: (b.subs || []).map(s => Object.assign(
            { id: s.id, name: s.name, sub: s.sub, count: s.count }, meta(s),
            { episodes: s.episodes.map(ep) })),
          episodes: (b.episodes || []).map(ep),
        })),
    };
    fs.writeFileSync(CACHE_FILE, JSON.stringify(slim));
  } catch { /* 只读挂载也能跑，只是重启后要重扫 */ }
}

function loadCache() {
  try {
    const j = JSON.parse(fs.readFileSync(CACHE_FILE, 'utf8'));
    if (j && j.dir === DIR && Array.isArray(j.books)) {
      MEM = { at: j.at || 0, dir: j.dir, books: j.books };
      scanAt = 0;                    // 让第一次访问触发一次真实对齐
      return true;
    }
  } catch { /* 没缓存 */ }
  return false;
}

function loadPos() {
  try { POS = JSON.parse(fs.readFileSync(POS_FILE, 'utf8')) || {}; } catch { POS = {}; }
}

function savePos() {
  try {
    fs.mkdirSync(path.dirname(POS_FILE), { recursive: true });
    fs.writeFileSync(POS_FILE, JSON.stringify(POS));
  } catch { /* ignore */ }
}

// ---------------------------------------------------------------- 对外接口

const encPath = rel => String(rel).split('/').map(encodeURIComponent).join('/');

/**
 * /m/<rel> 的落盘解析（有声书目录是**独立挂载**，和音乐目录不是一个根）。
 * 只允许落在 ABL_DIR 里 —— 防止 ../ 穿越。
 */
function absOf(rel) {
  const base = path.resolve(DIR);
  const abs = path.resolve(base, String(rel || ''));
  if (abs !== base && !abs.startsWith(base + path.sep)) return null;
  return abs;
}

/** 书列表（只给书名 + 集数 + 记忆，不吐集表 —— 点开才拉，见 episodes()） */
function books(q) {
  ensure(false);
  const kw = String(q || '').trim().toLowerCase();
  const out = [];
  for (const b of MEM.books) {
    if (kw) {
      const hay = (b.name + ' ' + (b.subs || []).map(s => s.name).join(' ')).toLowerCase();
      if (hay.indexOf(kw) < 0) continue;
    }
    const row = {
      id: b.id, name: b.name, kind: b.kind, count: b.count,
      // ★ 2026-11-04 晚：sidecar（封面/作者/演播/年份/短简介/总时长），没有就是空串
      title: b.title || '', author: b.author || '', narrator: b.narrator || '',
      year: b.year || '', cover: b.cover || '', intro: b.intro || '',
      durSec: b.durSec || 0,
      lastEp: 0, lastPos: 0, lastTitle: '',
    };
    const p = POS[b.id];
    if (p) { row.lastEp = p.ep || 0; row.lastPos = p.pos || 0; row.lastTitle = p.title || ''; }
    if (b.kind === 'coll') {
      row.subs = b.subs.map(s => {
        const sp = POS[s.id] || {};
        return { id: s.id, name: s.name, count: s.count,
                 title: s.title || '', author: s.author || '', narrator: s.narrator || '',
                 year: s.year || '', cover: s.cover || '', durSec: s.durSec || 0,
                 lastEp: sp.ep || 0, lastPos: sp.pos || 0, lastTitle: sp.title || '' };
      });
      // 父级记忆 = 子书里最近听过的那一份（父级要能同时看到各子项记忆）
      let best = null;
      for (const s of row.subs) {
        if (!s.lastEp) continue;
        if (!best || (POS[s.id].at || 0) > (POS[best.id].at || 0)) best = s;
      }
      if (best) {
        row.lastEp = best.lastEp; row.lastPos = best.lastPos; row.lastTitle = best.lastTitle;
        row.lastSub = best.name;
      }
    }
    out.push(row);
  }
  return { at: MEM.at, dir: MEM.dir, total: out.length, books: out };
}

/** 某本书/子书的集表（点击才拉；支持集数/标题搜索与分页） */
function episodes(id, opt) {
  ensure(false);
  const o = opt || {};
  const pid = parseId(id);
  if (!pid) return { error: 'id 无效' };
  let b = null, eps = null, name = '', sub = '', rec = null;
  for (const x of MEM.books) {
    if (pid.coll) {
      if (x.kind === 'coll' && x.name === pid.book) { b = x; rec = x; break; }
    } else if (x.kind === 'coll') {
      const s = (x.subs || []).find(z => z.id === id);
      if (s) { b = x; eps = s.episodes; name = x.name; sub = s.name; rec = s; break; }
    } else if (x.id === id) {
      b = x; eps = x.episodes; name = x.name; rec = x; break;
    }
  }
  if (!b) return { error: '没有这本书（可能盘上删了，重扫试试）' };
  if (pid.coll && !eps) {
    // 合集父级：只回子书清单，集表要再点一层
    return {
      id, name: b.name, kind: 'coll', total: b.count, count: b.count,
      title: b.title || '', author: b.author || '', narrator: b.narrator || '',
      year: b.year || '', cover: b.cover || '', desc: b.desc || '',
      subs: (b.subs || []).map(s => ({ id: s.id, name: s.name, count: s.count,
        title: s.title || '', cover: s.cover || '', durSec: s.durSec || 0 })),
      episodes: [],
    };
  }
  const kw = String(o.q || '').trim();
  let list = eps || [];
  if (kw) {
    const isNum = /^\d+$/.test(kw);
    list = list.filter(e => (isNum ? String(e.ep).indexOf(kw) === 0 : false)
      || String(e.title || '').toLowerCase().indexOf(kw.toLowerCase()) >= 0);
  }
  const size = Math.min(Math.max(parseInt(o.size || 100, 10) || 100, 10), 500);
  let page = Math.max(parseInt(o.page || 1, 10) || 1, 1);
  // 指定 ep ⇒ 定位到包含它的那一页（长书直接跳集）
  const epAt = parseInt(o.ep || 0, 10) || 0;
  if (epAt > 0) {
    const idx = list.findIndex(e => e.ep >= epAt);
    page = Math.floor((idx < 0 ? list.length - 1 : idx) / size) + 1;
  }
  const total = list.length;
  page = Math.min(page, Math.max(1, Math.ceil(total / size)));
  const start = (page - 1) * size;
  const slice = list.slice(start, start + size).map(e => ({
    ep: e.ep, title: e.title || '',
    dur: e.dur || 0,                    // ★ 章节时长（秒），来自 metadata.json 的 end-start
    // 本地书：直接给 /m/<rel>，播放器拿到就能播（服务端 lxrelay 那套取链完全不用）
    file: e.rel, url: '/m/' + encPath(e.rel), src: 'abl',
  }));
  const pp = POS[id] || {};
  // ★ rec = 当前这层（普通书=自己；合集子书=子书记录，别拿父级的封面/简介）
  const bk = rec || b;
  return {
    id, name, sub, kind: sub ? 'book' : (b.kind === 'coll' ? 'coll' : 'book'),
    title: bk.title || '', author: bk.author || '', narrator: bk.narrator || '',
    year: bk.year || '', cover: bk.cover || '', desc: bk.desc || '',
    total, page, size, hasMore: start + size < total,
    lastEp: pp.ep || 0, lastPos: pp.pos || 0, lastTitle: pp.title || '',
    episodes: slice,
  };
}

/** 记一次播放进度（按书/子书各一份；父级聚合看 books() 的 subs[].lastEp） */
function setPos(id, ep, pos, title) {
  if (!id) return { ok: false, error: 'id 无效' };
  const e = parseInt(ep || 0, 10) || 0;
  if (e <= 0) return { ok: false, error: 'ep 无效' };
  const p = Math.max(0, parseInt(pos || 0, 10) || 0);
  const cur = POS[id] || {};
  // 同一集：只往前推（回退多半是重播，别把记忆拉回去）
  if (cur.ep === e && p + 5 < (cur.pos || 0)) return { ok: true, kept: true, ep: e, pos: cur.pos || 0 };
  POS[id] = { ep: e, pos: p, title: String(title || '').slice(0, 80), at: Date.now() };
  const keys = Object.keys(POS);
  if (keys.length > POS_MAX) {
    keys.sort((a, b) => (POS[a].at || 0) - (POS[b].at || 0));
    for (const k of keys.slice(0, keys.length - POS_MAX)) delete POS[k];
  }
  savePos();
  return { ok: true, ep: e, pos: p };
}

/** 清一本书的记忆（父级清 = 连子书一起清） */
function clearPos(id) {
  const pid = parseId(id);
  let n = 0;
  if (pid && pid.coll) {
    for (const b of MEM.books) {
      if (b.kind !== 'coll' || b.name !== pid.book) continue;
      for (const s of b.subs || []) { if (POS[s.id]) { delete POS[s.id]; n++; } }
    }
  } else if (POS[id]) { delete POS[id]; n++; }
  if (n) savePos();
  return { ok: true, cleared: n };
}

/** 自检：为什么是空的（挂载没生效 / 一首都认不出 / 权限） */
function diag() {
  const d = { dir: DIR, exists: false, files: 0, books: 0, lastScanAgo: scanAt ? Math.round((Date.now() - scanAt) / 1000) : -1, sample: [] };
  try {
    const st = fs.statSync(DIR);
    d.exists = st.isDirectory();
    if (d.exists) {
      const out = [];
      walk(DIR, '', out, 0);
      d.files = out.length;
      d.sample = out.slice(0, 5).map(f => f.rel + '  ⇒  ' + JSON.stringify(parseName(f.name, f.rel.split('/').slice(-2, -1)[0])));
    }
  } catch (e) { d.err = String(e && e.message || e); }
  d.books = (MEM.books || []).length;
  return d;
}

loadCache();
loadPos();

module.exports = { scan, ensure, books, episodes, setPos, clearPos, diag, parseName, parseId, absOf, DIR, CACHE_FILE, POS_FILE };
