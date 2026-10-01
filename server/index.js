// server/index.js
require('dotenv').config();
const path = require('path');
const express = require('express');
const cors = require('cors');

const store = require('./store');
const telegram = require('./telegram');

const app = express();
const PORT = process.env.PORT || 10000;
const API_KEY = process.env.API_KEY || process.env.NODE_RED_API_KEY || '';

const TEMP_THRESHOLD = Number(process.env.TEMP_THRESHOLD || 50);
const HUMIDITY_THRESHOLD = Number(process.env.HUMIDITY_THRESHOLD || 20);
const SMOKE_THRESHOLD = Number(process.env.SMOKE_THRESHOLD || 600);

app.use(cors());
app.use(express.json({ limit: '100kb' }));
app.use(express.static(path.join(__dirname, '..', 'public')));

// Keep track of last alert state per node so we only notify on a transition
// into alert (same behaviour as the original Node-RED flow).
const wasAlerting = { node1: false, node2: false };

// ---------- Health check (useful for Render) ----------
app.get('/api/health', (req, res) => res.json({ ok: true, time: Date.now() }));

// ---------- ESP32 ingestion endpoint ----------
// Kept at /firedata (same path the Master firmware already posts to) so the
// existing master.ino only needs its URL updated, nothing else.
app.post('/firedata', async (req, res) => {
  const got = req.header('x-api-key');
  if (!API_KEY || got !== API_KEY) {
    return res.status(401).json({ ok: false, error: 'Unauthorized' });
  }

  const d = req.body || {};
  if (!['node1', 'node2'].includes(d.nodeId)) {
    return res.status(400).json({ ok: false, error: 'nodeId must be node1 or node2' });
  }

  const temperature = Number(d.temperature);
  const humidity = Number(d.humidity);
  const smoke_level = Number(d.smoke_level);
  if (![temperature, humidity, smoke_level].every(Number.isFinite)) {
    return res.status(400).json({ ok: false, error: 'Invalid sensor values' });
  }

  const alert =
    d.fire_detected === true ||
    temperature >= TEMP_THRESHOLD ||
    humidity <= HUMIDITY_THRESHOLD ||
    smoke_level >= SMOKE_THRESHOLD;

  const reading = store.saveReading(d.nodeId, { temperature, humidity, smoke_level, alert });

  // Fire alert only on the normal -> alert transition
  if (alert && !wasAlerting[d.nodeId]) {
    const message = telegram.alertMessage(d.nodeId, reading);
    const result = await telegram.sendMessage(message);
    store.addAlert({
      type: 'alert',
      nodeId: d.nodeId,
      message: `อุณหภูมิสูงผิดปกติ (${temperature} °C) / ควัน (MQ-2 = ${smoke_level})`,
      status: result.ok ? 'ส่งแล้ว' : 'ล้มเหลว'
    });
  } else if (!alert && wasAlerting[d.nodeId]) {
    store.addAlert({
      type: 'recovered',
      nodeId: d.nodeId,
      message: 'ค่ากลับสู่ภาวะปกติ',
      status: 'บันทึกแล้ว'
    });
  }
  wasAlerting[d.nodeId] = alert;

  res.json({ ok: true, nodeId: d.nodeId });
});

// ---------- Dashboard data endpoints (no auth: public read-only dashboard) ----------
app.get('/api/status', (req, res) => {
  const node1 = store.getNodeStatus('node1');
  const node2 = store.getNodeStatus('node2');
  const anyAlert = node1.status === 'ALERT' || node2.status === 'ALERT';
  const anyOnline = node1.online || node2.online;

  res.json({
    time: Date.now(),
    fire: anyAlert ? 'ALERT' : 'NORMAL',
    nodes: { node1, node2 },
    system: {
      node1Online: node1.online,
      node2Online: node2.online,
      espNow: anyOnline ? 'ทำงานปกติ' : 'ไม่มีสัญญาณ',
      wifi: anyOnline ? 'เชื่อมต่อ' : 'ขาดการเชื่อมต่อ',
      telegram: telegram.isConfigured() ? 'พร้อมใช้งาน' : 'ยังไม่ได้ตั้งค่า'
    }
  });
});

app.get('/api/history', (req, res) => {
  const minutes = Math.min(Number(req.query.minutes) || 1440, 48 * 60);
  const bucketMinutes = Number(req.query.bucket) || Math.max(1, Math.round(minutes / 144));
  res.json(store.getChartSeries(minutes, bucketMinutes));
});

app.get('/api/alerts', (req, res) => {
  const limit = Math.min(Number(req.query.limit) || 50, 300);
  res.json(store.getAlerts(limit));
});

// Manual "test alert" button on the dashboard
app.post('/api/test-telegram', async (req, res) => {
  const result = await telegram.sendMessage(
    '✅ ทดสอบการแจ้งเตือนจาก Forest Fire Detection Dashboard\nระบบแจ้งเตือนทำงานปกติ'
  );
  store.addAlert({
    type: 'test',
    nodeId: '-',
    message: 'ทดสอบส่งข้อความผ่าน Telegram',
    status: result.ok ? 'สำเร็จ' : 'ล้มเหลว: ' + (result.error || '')
  });
  res.json(result);
});

app.listen(PORT, () => {
  console.log(`Forest Fire Dashboard server listening on port ${PORT}`);
});
