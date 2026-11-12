'use strict';
/**
 * mg.js —— 落雪 musicSdk 咪咕（mg）模块的 Node 移植
 * 移植自 src/utils/musicSdk/mg/musicSearch.js
 *
 * 签名：MD5(`${keyword}${signatureMd5}yyapp2d16148780a1dcc7408e06336b98cfd50${deviceId}${time}`)
 *       deviceId / signatureMd5 都是脚本内硬编码常量（无需登录）。
 */
const { httpFetch } = require('./http');
const { decodeName, formatPlayTime, formatSingerName } = require('./util');
const { md5 } = require('./crypto');

const DEVICE_ID = '963B7AA0D21511ED807EE5846EC87D20';
const SIGNATURE_MD5 = '6cdc72a439cef99a3418d2a78aa28c73';
const UA = 'Mozilla/5.0 (Linux; U; Android 11.0.0; zh-cn; MI 11 Build/OPR1.170623.032) ' +
  'AppleWebKit/534.30 (KHTML, like Gecko) Version/4.0 Mobile Safari/534.30';

function createSignature(time, str) {
  return md5(`${str}${SIGNATURE_MD5}yyapp2d16148780a1dcc7408e06336b98cfd50${DEVICE_ID}${time}`);
}

async function search(keyword, limit = 20, page = 1) {
  const time = Date.now().toString();
  const searchSwitch = encodeURIComponent(JSON.stringify({
    song: 1, album: 0, singer: 0, tagSong: 1, mvSong: 0, bestShow: 1, songlist: 0, lyricSong: 0,
  }));
  const url = 'https://jadeite.migu.cn/music_search/v3/search/searchAll' +
    `?isCorrect=0&isCopyright=1&searchSwitch=${searchSwitch}` +
    `&pageSize=${limit}&text=${encodeURIComponent(keyword)}&pageNo=${page}&sort=0&sid=USS`;
  const { body } = await httpFetch(url, {
    headers: {
      uiVersion: 'A_music_3.6.1',
      deviceId: DEVICE_ID,
      timestamp: time,
      sign: createSignature(time, keyword),
      channel: '0146921',
      'User-Agent': UA,
    },
  });
  if (!body || body.code !== '000000') return [];
  const resultList = (body.songResultData && body.songResultData.resultList) || [];
  const seen = new Set();
  const out = [];
  for (const group of resultList) {
    for (const d of (group || [])) {
      if (!d.songId || !d.copyrightId || seen.has(d.copyrightId)) continue;
      seen.add(d.copyrightId);
      const types = [];
      for (const t of (d.audioFormats || [])) {
        const map = { PQ: '128k', HQ: '320k', SQ: 'flac', ZQ24: 'flac24bit' };
        if (map[t.formatType]) types.push({ type: map[t.formatType] });
      }
      let img = d.img3 || d.img2 || d.img1 || '';
      if (img && !/^https?:/.test(img)) img = 'http://d.musicapp.migu.cn' + img;
      out.push({
        rid: String(d.songId),
        title: decodeName(d.name || ''),
        artist: decodeName(formatSingerName(d.singerList)),
        album: decodeName(d.album || ''),
        duration: d.duration ? formatPlayTime(d.duration) : '',
        img,
        source: 'mg',
        copyrightId: d.copyrightId,
        types,
      });
    }
  }
  return out.slice(0, limit);
}

module.exports = { search };
