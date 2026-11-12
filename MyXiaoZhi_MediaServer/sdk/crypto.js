'use strict';
/**
 * crypto.js —— 落雪 `src/utils/nativeModules/crypto.ts` 的 Node 平替
 *
 * 原文件是 React Native 原生桥（CryptoModule.*），这里用 Node crypto 实现同名接口，
 * 让 musicSdk 的源码逻辑（如酷我榜单签名 wbdCrypto）可以原样跑。
 *
 * 约定（与原原生模块一致）：
 *   aesEncryptSync(text, key, vi, mode) -> base64 字符串
 *   aesDecryptSync(text /* base64 密文 *\/, key, vi, mode) -> 明文字符串
 */
const nodeCrypto = require('crypto');

const AES_MODE = {
  CBC_128_PKCS7Padding: 'AES/CBC/PKCS7Padding',
  ECB_128_NoPadding: 'AES',
};

const toKeyBuf = key => {
  // 酷我的 key 形如 'cFcnPcf6Kb85RC1y3V6M5A=='（base64，16 字节）
  const b = Buffer.from(String(key), 'base64');
  return b.length === 16 ? b : Buffer.from(String(key), 'utf8');
};

// NoPadding 要求明文长度是 16 的倍数，不足补 0（与常见原生实现一致）
const pad16 = buf => {
  const pad = (16 - (buf.length % 16)) % 16;
  return pad ? Buffer.concat([buf, Buffer.alloc(pad)]) : buf;
};

const aesEncryptSync = (text, key, vi, mode) => {
  const keyBuf = toKeyBuf(key);
  const inBuf = Buffer.from(String(text), 'utf8');
  if (mode === AES_MODE.ECB_128_NoPadding) {
    // ★ 坑：枚举名叫 NoPadding，但它的值是 'AES'，而 Java Cipher.getInstance("AES")
    //   默认是 AES/ECB/PKCS5Padding(=PKCS7) ⇒ 实际带填充。按无填充做会解出 wrong final block length。
    const c = nodeCrypto.createCipheriv('aes-128-ecb', keyBuf, null);
    return Buffer.concat([c.update(inBuf), c.final()]).toString('base64');
  }
  const ivBuf = Buffer.from(String(vi || '').padEnd(16, '\0').slice(0, 16), 'utf8');
  const c = nodeCrypto.createCipheriv('aes-128-cbc', keyBuf, ivBuf);
  return Buffer.concat([c.update(inBuf), c.final()]).toString('base64');
};

const aesDecryptSync = (text, key, vi, mode) => {
  const keyBuf = toKeyBuf(key);
  const data = Buffer.from(String(text), 'base64');
  if (mode === AES_MODE.ECB_128_NoPadding) {
    const d = nodeCrypto.createDecipheriv('aes-128-ecb', keyBuf, null);
    return Buffer.concat([d.update(data), d.final()]).toString('utf8');
  }
  const ivBuf = Buffer.from(String(vi || '').padEnd(16, '\0').slice(0, 16), 'utf8');
  const d = nodeCrypto.createDecipheriv('aes-128-cbc', keyBuf, ivBuf);
  return Buffer.concat([d.update(data), d.final()]).toString('utf8');
};

const md5 = s => nodeCrypto.createHash('md5').update(String(s), 'utf8').digest('hex');
const hashSHA1 = s => nodeCrypto.createHash('sha1').update(String(s), 'utf8').digest('hex');
const randomBytes = n => Buffer.from(nodeCrypto.randomBytes(n)).toString('hex');

module.exports = {
  AES_MODE, RSA_PADDING: { OAEPWithSHA1AndMGF1Padding: 'RSA/ECB/OAEPWithSHA1AndMGF1Padding', NoPadding: 'RSA/ECB/NoPadding' },
  aesEncryptSync, aesDecryptSync,
  aesEncrypt: async (...a) => aesEncryptSync(...a),
  aesDecrypt: async (...a) => aesDecryptSync(...a),
  rsaEncryptSync: () => '', rsaDecryptSync: () => '',
  rsaEncrypt: async () => '', rsaDecrypt: async () => '',
  md5, toMD5: md5, stringMd5: md5, hashSHA1, sha1: hashSHA1, randomBytes,
};
