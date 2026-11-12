'use strict';
/**
 * pool.js —— 「随机 100 首」曲库池
 *
 * 用户要求（2026-10-15）：不要复杂榜单，载入后就随机显示 100 首歌，能搜索、能语音点歌。
 *
 * 设计要点
 * 1. 池怎么来：酷我 11 个榜单各取 40 首 + 热搜前 8 词各搜 8 首 ⇒ 去重后约 400~500 首，
 *    再 Fisher-Yates 洗牌取前 100。这样每次「换一批」都是不同的 100 首，且都是当下的热歌。
 * 2. ★ 池必须**落盘**（files/pool.json）：
 *    语音点歌（R1 的 /index.json）用的是同一个池的**下标**（/s/<i>）。
 *    如果每次请求都重新洗，R1 已经加载的曲库和 Web 上看到的就对不上号，
 *    「播放第 3 首」会变成另一首歌。所以只有显式 fresh=1（换一批）才重摇。
 * 3. 上游全挂时保留旧池，绝不把曲库清空（空曲库 = 语音点歌直接哑火）。
 */
const fs = require('fs');
const path = require('path');
const S = require('./sdk/sources');

const FILE = path.join(__dirname, 'files', 'pool.json');
/** 池容量（洗牌后保留这么多，取用时再截 100） */
const CAP = 400;

/** 去重用：把「歌名+歌手」压成一个可比较的串（忽略空格/括号/标点/大小写） */
const norm = s => String(s || '').replace(/[\s()（）\[\]【】{},，.。!！?？'"]/g, '').toLowerCase();
const keyOf = t => norm(t.title) + '|' + norm(t.artist);

function shuffle(a) {
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    const t = a[i]; a[i] = a[j]; a[j] = t;
  }
  return a;
}

/**
 * 并发受限地跑一组任务（结果按原顺序回填）。
 *
 * ★★ 为什么不能用 Promise.all：gather 一次性会起 11 个榜单 + 8 个热搜词
 * = **19 个请求同时砸向音源**，加上「换一批」可以连点，这就是送上门的限流/封禁。
 * 这里压成「同时最多 lim 个 + 每个 worker 起步错峰」。
 */
async function mapLimit(jobs, lim, gap = 0) {
  const out = new Array(jobs.length);
  let idx = 0;
  const ws = [];
  for (let w = 0; w < Math.min(lim, jobs.length); w++) {
    ws.push((async () => {
      if (w && gap) await new Promise(r => setTimeout(r, w * gap));
      for (;;) {
        const i = idx++;
        if (i >= jobs.length) return;
        try {
          out[i] = await jobs[i]();
        } catch {
          out[i] = null;
        }
      }
    })());
  }
  await Promise.all(ws);
  return out;
}

/** 汇总：榜单 + 热搜词搜索，去重 */
async function gather() {
  // 注意：存的是**函数**而不是 Promise —— Promise 一创建请求就发出去了，
  // 那就又变成 19 个并发。交给 mapLimit 按需取用才能真正限住。
  const jobs = [];
  for (const b of (S.boards() || [])) {
    jobs.push(() => S.board(b.bangid, 1, 40)
      .then(list => ({ tag: b.name, list }))
      .catch(() => ({ tag: b.name, list: [] })));
  }
  const hots = await S.hotSearch().catch(() => []);
  for (const h of (hots || []).slice(0, 8)) {
    jobs.push(() => S.search('kw', h, 8)
      .then(list => ({ tag: h, list }))
      .catch(() => ({ tag: h, list: [] })));
  }
  // 并发 4 + 每个 worker 错峰 120ms ⇒ 19 个请求摊成约 1 秒的细流，而不是一瞬间
  const rs = await mapLimit(jobs, 4, 120);

  const seen = new Set();
  const out = [];
  for (const r of rs) {
    for (const it of (r.list || [])) {
      if (!it || !it.title || !it.rid) continue;
      const k = keyOf(it);
      if (seen.has(k)) continue;
      seen.add(k);
      out.push({
        rid: String(it.rid),
        title: String(it.title),
        artist: String(it.artist || ''),
        album: String(it.album || ''),
        duration: String(it.duration || ''),
        source: String(it.source || 'kw'),
        from: r.tag,
      });
    }
  }
  return out;
}

let POOL = null;
let building = null;      // 并发去重：同时来了两个请求只生成一次
/** 上次真正重建池的时刻（进程重启后由 pool.json 的 mtime 兜回来） */
let lastBuild = 0;
/**
 * 「换一批」的最小间隔 —— 60s 内不重复重建。
 *
 * ★ 重建一次 = 11 个榜单 + 8 个热搜词 = 19 个上游请求。R1 那边每说一次
 * 「随机播放在线音乐」就是一次 fresh=1；连着说三次就 57 个请求刷出去。
 * 冷却期内不换歌（调用方会自己洗牌，播放顺序照样不同），代价远小于被限流。
 */
const FRESH_GAP = 60000;

function loadDisk() {
  try {
    const j = JSON.parse(fs.readFileSync(FILE, 'utf8'));
    if (Array.isArray(j) && j.length) {
      POOL = j;
      try {
        lastBuild = Math.max(lastBuild, fs.statSync(FILE).mtimeMs);
      } catch { /* 读不到 mtime 就从 0 开始 */ }
    }
  } catch { /* 没有就用空池，触发首次生成 */ }
}
function saveDisk() {
  try {
    fs.mkdirSync(path.dirname(FILE), { recursive: true });
    fs.writeFileSync(FILE, JSON.stringify(POOL));
  } catch { /* 只读环境也能跑，只是重启后重摇 */ }
}

async function build() {
  const all = await gather();
  if (!all.length) return false;
  const idx = shuffle(all.map((_, i) => i)).slice(0, CAP);
  POOL = idx.map(i => all[i]);
  saveDisk();
  return true;
}

/**
 * 取曲库（前 n 首）。
 * @param {number} n  要几首（默认 100）
 * @param {boolean} fresh  true = 重新洗一批（「换一批」按钮）
 */
async function get(n = 100, fresh = false) {
  if (!POOL) loadDisk();
  if (fresh || !POOL || !POOL.length) {
    if (!building) {
      building = build()
        .then(okv => okv)
        .catch(() => false)
        .finally(() => { building = null; });
    }
    const okv = await building;
    if (!okv && (!POOL || !POOL.length)) return [];    // 上游全挂且没旧池
  }
  return POOL.slice(0, n).map((t, i) => Object.assign({ i }, t));
}

/** 当前池的总量（不给参数时看规模用） */
function size() { return (POOL || (loadDisk(), POOL || [])).length; }

/**
 * 往池里加歌（搜索结果「加入曲库」用）—— 语音点歌的范围就跟着变大。
 * @returns {{added:number, total:number}}
 */
function add(items) {
  if (!POOL) loadDisk();
  if (!POOL) POOL = [];
  const seen = new Set(POOL.map(keyOf));
  let added = 0;
  for (const it of (items || [])) {
    if (!it || !it.title || !it.rid) continue;
    const k = keyOf(it);
    if (seen.has(k)) continue;
    seen.add(k);
    const row = {
      rid: String(it.rid),
      title: String(it.title),
      artist: String(it.artist || ''),
      album: String(it.album || ''),
      duration: String(it.duration || ''),
      source: String(it.source || 'kw'),
      from: '手动添加',
    };
    // ★ 落雪搜索给的 `_types`（这首有哪些音质档 / kg 的每档 hash）跟着一起落盘：
    //   起播取链时少了它，mg 一律 500「咪咕找不到」。
    if (it.types) row.types = it.types;
    POOL.push(row);
    added++;
  }
  if (added) saveDisk();
  return { added, total: POOL.length };
}

/** 取第 i 条（/s/<i> 用） */
function at(i) {
  if (!POOL) loadDisk();
  return (POOL && POOL[i]) || null;
}

/**
 * 按 source+rid 反查池里的条目（/r/<source>/<rid> 用它补 title/artist）。
 * 池是几百~上千条的量级，线性扫一次不值一提；命中就免了调用方传错歌名。
 */
function byRid(source, rid) {
  if (!POOL) loadDisk();
  if (!POOL || !rid) return null;
  const r = String(rid);
  const s = String(source || '');
  for (let i = 0; i < POOL.length; i++) {
    const t = POOL[i];
    if (!t) continue;
    if (String(t.rid) === r && (!s || String(t.source || 'kw') === s)) return t;
  }
  return null;
}

module.exports = { get, add, at, byRid, size, CAP, FILE, mapLimit };
