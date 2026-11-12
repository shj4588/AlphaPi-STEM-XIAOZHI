'use strict';
/**
 * tx.js —— 落雪 musicSdk QQ音乐（tx）模块的 Node 移植
 * 移植自 src/utils/musicSdk/tx/{musicSearch.js, utils/index.js, utils/crypto.js}
 *
 * 签名 zzcSign（★ 纯 JS，无原生依赖）：
 *   hash  = SHA1(JSON.stringify(data))                     // 40 位 hex
 *   part1 = 取 hash 的第 [23,14,6,36,16,40,7,19] 位
 *   part2 = 取 hash 的第 [16,1,32,12,19,27,8,5] 位
 *   part3 = SCRAMBLE_VALUES[i] ^ parseInt(hash.slice(i*2,i*2+2),16)   // 20 字节
 *   sign  = `zzc${part1}${base64(part3) 去掉 /+=}${part2}`.toLowerCase()
 * POST https://u.y.qq.com/cgi-bin/musics.fcg?sign=<sign>   (application/json)
 */
const { httpFetch } = require('./http');
const { decodeName, formatPlayTime, formatSingerName } = require('./util');
const { hashSHA1 } = require('./crypto');

const PART_1_INDEXES = [23, 14, 6, 36, 16, 40, 7, 19];
const PART_2_INDEXES = [16, 1, 32, 12, 19, 27, 8, 5];
const SCRAMBLE_VALUES = [89, 39, 179, 150, 218, 82, 58, 252, 177, 52, 186, 123, 120, 64, 242, 133, 143, 161, 121, 179];
const UA = 'QQMusic 14090508(android 12)';

const pickHashByIdx = (hash, indexes) => indexes.map(i => hash[i]).join('');

async function zzcSign(text) {
  const hash = await hashSHA1(text);
  const part1 = pickHashByIdx(hash, PART_1_INDEXES);
  const part2 = pickHashByIdx(hash, PART_2_INDEXES);
  const part3 = SCRAMBLE_VALUES.map((v, i) => v ^ parseInt(hash.slice(i * 2, i * 2 + 2), 16));
  const b64Part = Buffer.from(part3).toString('base64').replace(/[\\/+=]/g, '');
  return `zzc${part1}${b64Part}${part2}`.toLowerCase();
}

// PC 客户端版 searchid（musicSearch.js getSearchId）
function getSearchId() {
  let guid = '';
  for (let i = 0; i < 32; i++) guid += Math.floor(Math.random() * 16).toString(16);
  return guid.toUpperCase() + String(Math.floor(Math.random() * 100000)).padStart(5, '0');
}

async function signRequest(data) {
  const sign = await zzcSign(JSON.stringify(data));
  const { body } = await httpFetch(`https://u.y.qq.com/cgi-bin/musics.fcg?sign=${sign}`, {
    method: 'post',
    headers: { 'User-Agent': UA },
    body: data,
  });
  return body;
}

async function search(keyword, limit = 50, page = 1) {
  const data = {
    comm: {
      _channelid: '0', _os_version: '6.2.9200-2', ct: '19', cv: '2151',
      guid: '1F70E520B2EAA7D25E11760783C53CA9', patch: '118',
      psrf_access_token_expiresAt: 0, psrf_qqaccess_token: '',
      psrf_qqopenid: '', psrf_qqunionid: '', tmeAppID: 'qqmusic',
      tmeLoginType: 0, uin: '0', wid: '7223299733393904640',
    },
    'music.search.SearchCgiService': {
      module: 'music.search.SearchCgiService',
      method: 'DoSearchForQQMusicDesktop',
      param: {
        grp: 1, num_per_page: limit, page_num: page, query: keyword,
        remoteplace: 'txt.newclient.top', search_type: 0, searchid: getSearchId(),
      },
    },
  };
  const resp = await signRequest(data);
  const req = (resp && resp['music.search.SearchCgiService']) || (resp && resp.req);
  if (!req || resp.code !== 0 || req.code !== 0) return [];
  const list = (req.data && req.data.body && req.data.body.song && req.data.body.song.list) || [];
  const out = [];
  for (const item of list) {
    if (!item.file || !item.file.media_mid) continue;
    const albumId = (item.album && item.album.mid) || '';
    const types = [];
    if (item.file.size_128mp3) types.push({ type: '128k' });
    if (item.file.size_320mp3) types.push({ type: '320k' });
    if (item.file.size_flac) types.push({ type: 'flac' });
    if (item.file.size_hires) types.push({ type: 'flac24bit' });
    out.push({
      rid: String(item.mid || ''),
      title: decodeName(item.title || ''),
      artist: decodeName(formatSingerName(item.singer, 'name')),
      album: decodeName((item.album && item.album.name) || ''),
      duration: item.interval ? formatPlayTime(item.interval) : '',
      img: (albumId && albumId !== '空')
        ? `https://y.gtimg.cn/music/photo_new/T002R500x500M000${albumId}.jpg`
        : (item.singer && item.singer[0] && item.singer[0].mid
          ? `https://y.gtimg.cn/music/photo_new/T001R500x500M000${item.singer[0].mid}.jpg` : ''),
      source: 'tx',
      songId: item.id,
      strMediaMid: item.file.media_mid,
      types,
    });
  }
  return out.slice(0, limit);
}

module.exports = { search, zzcSign, signRequest };
