'use strict';
/**
 * sources.js —— 音源注册表（统一契约）
 *
 * 每个源至少提供 search(keyword, limit, page) -> [{rid,title,artist,album,duration,img,source}]
 * kw 额外提供 board / boards / lyric / pic / hotSearch（移植自 musicSdk/kw 全模块）。
 * 新增一个源 = 在这里加一行 + 写一个 sdk/<id>.js，server 与 UI 自动支持。
 */
const kw = require('./kw');
const kg = require('./kg');
const wy = require('./wy');
const tx = require('./tx');
const mg = require('./mg');

const REG = {
  kw: { name: '酷我', caps: ['search', 'board', 'lyric', 'pic', 'hot'], mod: kw },
  kg: { name: '酷狗', caps: ['search'], mod: kg },
  wy: { name: '网易云', caps: ['search'], mod: wy },
  tx: { name: 'QQ音乐', caps: ['search'], mod: tx },
  mg: { name: '咪咕', caps: ['search'], mod: mg },
};

const ids = () => Object.keys(REG);

const meta = () => ids().map(id => ({ id, name: REG[id].name, caps: REG[id].caps }));

async function search(source, keyword, limit = 30, page = 1) {
  const s = REG[source];
  if (!s || !s.mod.search) throw new Error('不支持的源: ' + source);
  const list = await s.mod.search(keyword, limit, page);
  return (list || []).map(x => Object.assign({ source }, x));
}

// 并发聚合搜索：任一源失败不影响其它源
async function searchAll(keyword, limit = 20, page = 1, only = null) {
  const targets = only ? ids().filter(i => only.includes(i)) : ids();
  const jobs = targets.map(id =>
    search(id, keyword, limit, page)
      .then(list => ({ id, ok: true, list }))
      .catch(e => ({ id, ok: false, list: [], error: String(e && e.message || e).slice(0, 120) })));
  const rs = await Promise.all(jobs);
  const list = [];
  const errors = [];
  for (const r of rs) {
    if (!r.ok) errors.push({ source: r.id, name: REG[r.id].name, error: r.error });
    for (const it of r.list) list.push(it);
  }
  return { list, errors };
}

module.exports = { REG, ids, meta, search, searchAll,
  // 透出 kw 的扩展能力（榜单/歌词等目前只有酷我移植完整）
  boards: kw.boards, board: kw.board, lyric: kw.lyric, pic: kw.pic, hotSearch: kw.hotSearch,
  // ★ 内置直链兜底（不依赖第三方音源后端）
  kwDirectUrl: kw.directUrl,        // antiserver：酷我，实测多为「试听片段/客户端提示音」
  wyDirectUrl: wy.directUrl,        // 网易明文 player/url：实测出完整 320k
  /**
   * ★ 跨源兜底：拿「歌名+歌手」去网易云找可播的一首取真直链。
   * 必要性：酷我 antiserver 只给 11 秒提示音（用户实听到「当前音乐仅在酷我音乐客户端播放」），
   * 而榜单/酷我搜索结果全是 kw rid —— 走跨源才能拿到完整歌曲。
   *
   * 返回对象（不是字符串）：{url, kind:'exact'|'alt', song, br, size}
   *   kind='exact' 歌手对得上；kind='alt' 只有歌名对得上（同名其它版本/翻唱），
   *   但至少是完整曲，好过酷我的试听片段。
   */
  crossUrl: (title, artist, opt) => wy.matchUrl(title, artist, opt),

  /**
   * ★★ 酷狗跨源（2026-10-15 新增，实测优于网易跨源）
   * 决定性实测：
   *   周杰伦《晴天》酷我 antiserver 只给 181,521 字节（11 秒试听提示音），
   *   酷狗 playInfo 的 backup_url 给 3.3MB 完整曲 —— 6/6 全部命中完整曲。
   *   且酷狗给的一律是「原唱歌手」的版本（录音室版要 VIP 时给 Live / 演唱会版），
   *   比网易跨源给的「别人翻唱」更接近用户要的原版。
   *
   * 说明：录音室原版在酷狗多为 VIP（playInfo 返回 errcode≠0 或缺 url），
   * 此时 matchUrl 会按打分退到 Live / 演唱会版，绝不会退回试听片段。
   */
  kgCrossUrl: (title, artist, opt) => kg.matchUrl(title, artist, opt),
  /** 酷狗限流冷却剩余毫秒（>0 表示暂时别打酷狗） */
  kgCooldownLeft: kg.cooldownLeft,

  /**
   * ★★ 网易登录态 cookie —— 拿到原版直链的钥匙
   * 填自己的 `MUSIC_U=...`（浏览器登录 music.163.com 后从 DevTools 复制），
   * 网易就会对 VIP / 无版权曲目返回 320k 原版直链，而不是 -110。
   */
  wySetCookie: wy.setCookie,
  wyGetCookie: wy.getCookie,
  wyHasCookie: wy.hasCookie,
};
