'use strict';
/**
 * http.js —— 落雪 `src/utils/request.js` 的 Node 平替
 *
 * 原文件只 import `react-native-background-timer`（超时计时），这里用 AbortSignal.timeout 平替。
 * 保留与落雪一致的调用约定：
 *   - options.form   → application/x-www-form-urlencoded（request.js:112）
 *   - options.body   → 对象时按 application/json 序列化（request.js:136）
 *   - format         → 默认 'json'：成功解析则 resp.body 为对象，否则为原始文本（request.js:102,201）
 */
const DEFAULT_UA = 'Mozilla/5.0 (Linux; Android 9) kwplayer_ar_9.2.2.1';

async function httpFetch(url, options = {}) {
  const {
    method = 'get',
    headers = {},
    form = null,
    body = null,
    timeout = 15000,
    format = 'json',
  } = options;

  const h = Object.assign({ Accept: 'application/json', 'User-Agent': DEFAULT_UA }, headers);
  let payload;

  if (String(method).toLowerCase() === 'post') {
    if (form) {
      h['Content-Type'] = 'application/x-www-form-urlencoded';
      payload = Object.entries(form)
        .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
        .join('&');
    } else {
      h['Content-Type'] = h['Content-Type'] || 'application/json';
      payload = typeof body === 'string' ? body : JSON.stringify(body);
    }
  }

  const resp = await fetch(url, {
    method: method.toUpperCase(),
    headers: h,
    body: payload,
    redirect: 'follow',
    signal: AbortSignal.timeout(timeout),
  });

  const text = await resp.text();
  let parsed = text;
  if (format === 'json') {
    try { parsed = JSON.parse(text); } catch { parsed = text; }
  }
  return {
    statusCode: resp.status,
    headers: Object.fromEntries(resp.headers.entries()),
    body: parsed,
    raw: text,
  };
}

// url 安全的 query 拼接（保留落雪里手拼 URL 的原貌用）
const qs = obj => Object.entries(obj)
  .map(([k, v]) => `${k}=${encodeURIComponent(v)}`)
  .join('&');

module.exports = { httpFetch, qs, DEFAULT_UA };
