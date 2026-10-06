// server/telegram.js
// Minimal Telegram Bot API wrapper. Uses Node's built-in fetch (Node 18+).

function isConfigured() {
  return !!(process.env.TELEGRAM_BOT_TOKEN && process.env.TELEGRAM_CHAT_ID);
}

async function sendMessage(text) {
  if (!isConfigured()) {
    return { ok: false, error: 'TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID not set' };
  }
  const token = process.env.TELEGRAM_BOT_TOKEN;
  const chatId = process.env.TELEGRAM_CHAT_ID;
  const url = `https://api.telegram.org/bot${token}/sendMessage`;

  try {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ chat_id: chatId, text })
    });
    const data = await res.json();
    if (!data.ok) return { ok: false, error: data.description || 'Telegram API error' };
    return { ok: true };
  } catch (err) {
    return { ok: false, error: err.message };
  }
}

function alertMessage(nodeId, reading, trend, smokeInfo) {
  // trend มาจาก store.trackTrend(): { consecutiveBreaches, required, rising }
  const trendLine = trend
    ? `เกินเกณฑ์ติดต่อกัน: ${trend.consecutiveBreaches}/${trend.required} รอบ` +
      (trend.rising === false
        ? ' (เริ่มลดลง — อาจเป็นไฟที่มีคนควบคุมอยู่ แต่ยังควรตรวจสอบ)'
        : ' (ยังเพิ่มขึ้นต่อเนื่อง — เสี่ยงลุกลาม)') + '\n'
    : '';

  // smokeInfo มาจาก store.getEffectiveSmokeThreshold(): { threshold, usingBaseline, baseline }
  const smokeLine = smokeInfo
    ? (smokeInfo.usingBaseline
        ? `เกณฑ์ควันของ node นี้: ${smokeInfo.threshold} (baseline ปกติ ${smokeInfo.baseline} x อัตรา)\n`
        : `เกณฑ์ควันของ node นี้: ${smokeInfo.threshold} (ค่าเริ่มต้น — ยังเรียนรู้ baseline ไม่ครบ)\n`)
    : '';

  return (
    '🔥 แจ้งเตือนระบบตรวจจับไฟป่า\n' +
    `Node: ${nodeId}\n` +
    `อุณหภูมิ: ${reading.temperature} °C\n` +
    `ความชื้น: ${reading.humidity} %\n` +
    `ระดับควัน (ADC): ${reading.smoke_level}\n` +
    smokeLine +
    trendLine +
    'กรุณาตรวจสอบพื้นที่ (ระบบแจ้งเตือนเบื้องต้น ไม่ใช่การยืนยันเหตุการณ์)'
  );
}

module.exports = { isConfigured, sendMessage, alertMessage };
