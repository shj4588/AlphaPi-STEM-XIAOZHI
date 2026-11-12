'use strict';
/**
 * kw.js —— 落雪 musicSdk 酷我（kw）模块的 Node 移植
 *
 * 移植自 lyswhut/lx-music-mobile  src/utils/musicSdk/kw/：
 *   musicSearch.js  搜索（http://search.kuwo.cn/r.s）
 *   leaderboard.js  榜单（https://wbd.kuwo.cn/api/bd/bang/bang_info，需 AES 签名）
 *   lyric.js        歌词（http://m.kuwo.cn/newh5/singles/songinfoandlrc）
 *   pic.js          封面（http://artistpicserver.kuwo.cn/pic.web）
 *   hotSearch.js    热搜词（http://hotword.kuwo.cn/hotword.s）
 *   util.js         wbdCrypto（榜单签名）、formatSinger
 *
 * 全部直连酷我公开接口，无需 cookie、无需 key。
 */
const { AES_MODE, aesEncryptSync, aesDecryptSync, md5 } = require('./crypto');
const { httpFetch, DEFAULT_UA } = require('./http');
const { decodeName, formatPlayTime } = require('./util');

const UA = DEFAULT_UA;
const UA_DALVIK = 'Dalvik/2.1.0 (Linux; U; Android 9;)';

// 对应 kw/util.js formatSinger
const formatSinger = raw => String(raw || '').replace(/&/g, '、');

// ---------------------------------------------------------------- 榜单签名
// 移植自 kw/util.js 的 wbdCrypto
const wbdCrypto = {
  aesKey: 'cFcnPcf6Kb85RC1y3V6M5A==',
  aesIv: '',
  appId: 'y67sprxhhpws',
  decodeData(base64Result) {
    return JSON.parse(aesDecryptSync(decodeURIComponent(base64Result), this.aesKey, this.aesIv, AES_MODE.ECB_128_NoPadding));
  },
  createSign(data, time) {
    return md5(`${this.appId}${data}${time}`).toUpperCase();
  },
  buildParam(jsonData) {
    // ★ 实测校正：源码这里是 Buffer.from(JSON.stringify(..)).toString('base64')，
    //   但服务端会回 DECRYPT_ERROR。实际要的明文是「原始 JSON 字符串」，
    //   配合 AES/ECB/PKCS7（Java 'AES' 默认）。四种组合穷举只有这一种通过。
    const data = JSON.stringify(jsonData);
    const time = Date.now();
    const encodeData = aesEncryptSync(data, this.aesKey, this.aesIv, AES_MODE.ECB_128_NoPadding);
    const sign = this.createSign(encodeData, time);
    return `data=${encodeURIComponent(encodeData)}&time=${time}&appId=${this.appId}&sign=${sign}`;
  },
};

// ---------------------------------------------------------------- 搜索
// 移植自 kw/musicSearch.js 的真实参数（★ 必须带 rformat=json，否则返回老式纯文本）
const SEARCH_URL = 'http://search.kuwo.cn/r.s' +
  '?client=kt&all={kw}&pn={pn}&rn={rn}&uid=794762570' +
  '&ver=kwplayer_ar_9.2.2.1&vipver=1&show_copyright_off=1&newver=1&ft=music' +
  '&cluster=0&strategy=2012&encoding=utf8&rformat=json&vermerge=1&mobi=1&issubtitle=1';

async function search(keyword, limit = 20, page = 1) {
  const url = SEARCH_URL
    .replace('{kw}', encodeURIComponent(keyword))
    .replace('{pn}', String(page - 1))
    .replace('{rn}', String(limit));
  const { body } = await httpFetch(url);
  return (body.abslist || []).map(it => ({
    rid: String(it.MUSICRID || '').replace('MUSIC_', ''),
    title: decodeName(it.NAME),
    artist: formatSinger(decodeName(it.ARTIST)),
    album: decodeName(it.ALBUM),
    duration: it.DURATION ? formatPlayTime(parseInt(it.DURATION, 10)) : '',
  })).filter(x => x.rid);
}

// ---------------------------------------------------------------- 榜单
// 移植自 kw/leaderboard.js 的 list（官方默认榜单）
const BOARDS = [
  { id: 'kwbiaosb', name: '飙升榜', bangid: 93 },
  { id: 'kwregb', name: '热歌榜', bangid: 16 },
  { id: 'kwhuiyb', name: '会员榜', bangid: 145 },
  { id: 'kwdouyb', name: '抖音榜', bangid: 158 },
  { id: 'kwqsb', name: '趋势榜', bangid: 187 },
  { id: 'kwhuaijb', name: '怀旧榜', bangid: 26 },
  { id: 'kwhuayb', name: '华语榜', bangid: 104 },
  { id: 'kwyueyb', name: '粤语榜', bangid: 182 },
  { id: 'kwoumb', name: '欧美榜', bangid: 22 },
  { id: 'kwhanyb', name: '韩语榜', bangid: 184 },
  { id: 'kwriyb', name: '日语榜', bangid: 183 },
];

const boards = () => BOARDS.map(b => ({ ...b }));

async function board(bangid, page = 1, limit = 100) {
  const requestBody = {
    uid: '', devId: '', sFrom: 'kuwo_sdk', user_type: 'AP',
    carSource: 'kwplayercar_ar_6.0.1.0_apk_keluze.apk',
    id: Number(bangid), pn: page - 1, rn: limit,
  };
  const url = `https://wbd.kuwo.cn/api/bd/bang/bang_info?${wbdCrypto.buildParam(requestBody)}`;
  const { body } = await httpFetch(url, { cache: 'default' });
  const raw = wbdCrypto.decodeData(body);
  const data = raw.data || {};
  return ((data.musiclist) || []).map(item => ({
    rid: String(item.id),
    title: decodeName(item.name),
    artist: formatSinger(decodeName(item.artist)),
    album: decodeName(item.album),
    duration: item.duration ? formatPlayTime(parseInt(item.duration, 10)) : '',
    img: item.pic || '',
    source: 'kw',
  }));
}

// ---------------------------------------------------------------- 歌词
// 移植自 kw/lyric.js
async function lyric(songId) {
  const { body } = await httpFetch(`http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=${songId}`);
  const d = body && body.data;
  const list = d && d.lrclist;
  if (!list || !list.length) {
    return { lines: [], lrc: '', info: (d && d.songinfo) || null };
  }
  const lines = list
    .map(l => ({ time: parseFloat(l.time), text: decodeName(l.lineLyric) }))
    .filter(l => !isNaN(l.time))
    .sort((a, b) => a.time - b.time);
  const fmt = t => {
    const m = Math.floor(t / 60);
    const s = (t % 60).toFixed(2);
    return (m < 10 ? '0' + m : m) + ':' + (s < 10 ? '0' + s : s);
  };
  const lrc = lines.map(l => `[${fmt(l.time)}]${l.text}`).join('\n');
  return { lines, lrc, info: d.songinfo || null };
}

// ---------------------------------------------------------------- 封面 / 热搜
async function pic(songId) {
  const { body } = await httpFetch(
    `http://artistpicserver.kuwo.cn/pic.web?corp=kuwo&type=rid_pic&pictype=500&size=500&rid=${songId}`);
  return /^http/.test(body) ? body : null;
}

async function hotSearch() {
  const { body, statusCode } = await httpFetch(
    'http://hotword.kuwo.cn/hotword.s?prod=kwplayer_ar_9.3.0.1&corp=kuwo&newver=2' +
    '&vipver=9.3.0.1&source=kwplayer_ar_9.3.0.1_40.apk&p2p=1&notrace=0&uid=0' +
    '&plat=kwplayer_ar&rformat=json&encoding=utf8&tabid=1',
    { headers: { 'User-Agent': UA_DALVIK } });
  if (statusCode !== 200 || !body || body.status !== 'ok') return [];
  return (body.tagvalue || []).map(i => i.key).filter(Boolean);
}

// ---------------------------------------------------------------- 内置直链兜底
/**
 * ★ 内置取链（不依赖任何第三方音源后端）：
 *   antiserver 是酷自己家的老转链接口，实测（2026-10-15）无需任何鉴权：
 *   `http://antiserver.kuwo.cn/anti.s?type=convert_url&rid=MUSIC_<rid>&format=<fmt>&response=url`
 *   → 响应体就是纯文本直链（https kw-bj.kuwo.cn/...mp3，HEAD 200 audio/mpeg）。
 *   高音质参数 2000kmp3 也通（歌曲本身没有该档时自动回落到实际存在的档）。
 *   ape/flac 会返回 "refuse request!"（需登录，不可用）。
 * 用途：第三方音源后端（ikunshare / sayqz）不可达时的兜底，仅限 kw rid。
 */
async function directUrl(rid) {
  for (const fmt of ['2000kmp3', 'mp3']) {
    try {
      const r = await httpFetch(
        'http://antiserver.kuwo.cn/anti.s?type=convert_url&rid=MUSIC_' + encodeURIComponent(rid) +
        '&format=' + fmt + '&response=url',
        { timeout: 15000 });
      const u = String(r.raw).trim();
      if (r.statusCode === 200 && /^https?:\/\//.test(u)) return u;
      // "refuse request!" 等非直链响应 → 试下一档
    } catch { /* 网络抖动 → 试下一档 */ }
  }
  return '';
}

module.exports = {
  search, boards, board, lyric, pic, hotSearch, directUrl,
  httpFetch, decodeName, formatPlayTime, wbdCrypto,
};
