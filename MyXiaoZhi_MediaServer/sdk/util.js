'use strict';
/**
 * util.js —— 落雪 `src/utils/index.ts` + `utils/musicSdk/utils.js` 的通用工具
 * 移植：decodeName / formatPlayTime / sizeFormate / formatSingerName
 */

// HTML 实体解码（落雪 decodeName）
function decodeName(str) {
  if (!str) return '';
  return String(str)
    .replace(/&#x([0-9a-f]+);/gi, (m, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&#(\d+);/g, (m, d) => String.fromCodePoint(Number(d)))
    .replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&nbsp;/g, ' ');
}

// 秒 -> mm:ss
function formatPlayTime(time) {
  const t = Math.round(Number(time) || 0);
  const m = Math.floor(t / 60);
  const s = Math.floor(t % 60);
  return (m < 10 ? '0' + m : m) + ':' + (s < 10 ? '0' + s : s);
}

// 字节 -> 人类可读
function sizeFormate(size) {
  if (!size) return null;
  const n = Number(size);
  if (n < 1024) return n + 'B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(2) + 'K';
  if (n < 1024 * 1024 * 1024) return (n / 1024 / 1024).toFixed(2) + 'M';
  return (n / 1024 / 1024 / 1024).toFixed(2) + 'G';
}

// 歌手数组 -> "A、B"（utils/musicSdk/utils.js formatSingerName）
function formatSingerName(singers, key = 'name') {
  if (!singers) return '';
  if (typeof singers === 'string') return singers;
  return (Array.isArray(singers) ? singers : [singers])
    .map(s => (typeof s === 'string' ? s : (s && s[key]) || ''))
    .filter(Boolean)
    .join('、');
}

/**
 * ★ 歌手别名表（2026-10-15 实测踩坑）
 * 网易云对港台歌手常只标英文名：搜「周杰伦 晴天」返回的是「晴天 — Jay」，
 * 而匹配逻辑做的是字符串包含，'jay' 与 '周杰伦' 互不包含 ⇒ 被误判成
 * 「别人唱的同名版本」，UI 因此显示"原曲无版权，播的是翻唱" —— 其实是原唱。
 * 这里给高频歌手补英文名（含繁体），跨源匹配时一并认。
 */
const ARTIST_ALIAS = {
  周杰伦: ['jay', 'jaychou', '周杰倫'],
  陈奕迅: ['eason', 'easonchan', '陳奕迅', '陈奕迅'],
  林俊杰: ['jjlin', 'jj'],
  邓紫棋: ['gem', 'g.e.m', '鄧紫棋'],
  蔡依林: ['jolin', 'jolintsai'],
  王力宏: ['leehom', 'leehomwang'],
  张惠妹: ['amei', 'a-mei', '張惠妹'],
  五月天: ['mayday'],
  薛之谦: ['joker', 'jokerxue', '薛之謙'],
  张艺兴: ['lay', 'layzhang', '張藝興'],
  鹿晗: ['luhan'],
  李荣浩: ['ronghao', 'li ronghao'],
  华晨宇: ['huachenyu'],
  王一博: ['yibo', 'wang yibo'],
  萧敬腾: ['jam', 'jamhsiao', '蕭敬騰'],
  张杰: ['jason', 'jasonzhang', '張傑'],
  周深: ['zhoushen', 'charlie'],
  毛不易: ['maobuyi'],
  张国荣: ['leslie', 'lesliecheung', '張國榮'],
  王菲: ['fayewong', 'faye', '王靖雯'],
  张学友: ['jacky', 'jackycheung', '張學友'],
  刘德华: ['andy', 'andylau', '劉德華'],
  黎明: ['leon', 'leonlai'],
  郭富城: ['aaron', 'aaronkwok'],
  孙燕姿: ['stefanie', 'stefaniesun', '孫燕姿'],
  梁静茹: ['fish', 'fishleong', '梁靜茹'],
  容祖儿: ['joey', 'joeyyung', '容祖兒'],
  杨丞琳: ['rainie', 'rainieyang'],
  田馥甄: ['hebe', 'hetian'],
  任家萱: ['selina'],
  陈嘉桦: ['ella'],
  SHE: ['she'],
  TFBOYS: ['tfboys', '加油男孩'],
  时代少年团: ['时代少年团', 'teens'],
};

/**
 * 歌手是否「对得上」（跨源匹配共用）
 * @param {string} want    目标歌手（如「周杰伦」）
 * @param {string} got     候选歌手（如「Jay Chou」）
 */
function artistMatch(want, got) {
  if (!want) return true;
  const norm = s => String(s || '').replace(/[\s·・]/g, '').toLowerCase();
  const a = norm(want);
  const b = norm(got);
  if (!a || !b) return false;
  if (b.includes(a) || a.includes(b)) return true;
  // 别名双向认：目标/候选任一侧命中对方的英文名就算同一个人
  const al = ARTIST_ALIAS[want] || ARTIST_ALIAS[String(want).replace(/\s/g, '')] || [];
  if (al.some(x => b.includes(String(x).replace(/[\s·]/g, '').toLowerCase()))) return true;
  for (const k of Object.keys(ARTIST_ALIAS)) {
    const kx = norm(k);
    if (kx === a || kx === b) {
      const l = ARTIST_ALIAS[k];
      if (l.some(x => b.includes(String(x).replace(/[\s·]/g, '').toLowerCase()))) return true;
    }
  }
  return false;
}

module.exports = {
  decodeName, formatPlayTime, sizeFormate, formatSingerName,
  artistMatch, ARTIST_ALIAS,
};
