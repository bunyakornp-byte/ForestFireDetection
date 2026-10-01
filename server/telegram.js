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

function alertMessage(nodeId, reading) {
  return (
    '🔥 แจ้งเตือนระบบตรวจจับไฟป่า\n' +
    `Node: ${nodeId}\n` +
    `อุณหภูมิ: ${reading.temperature} °C\n` +
    `ความชื้น: ${reading.humidity} %\n` +
    `ระดับควัน (ADC): ${reading.smoke_level}\n` +
    'กรุณาตรวจสอบพื้นที่'
  );
}

module.exports = { isConfigured, sendMessage, alertMessage };
