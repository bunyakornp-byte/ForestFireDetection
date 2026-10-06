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

// จำนวนครั้งที่ต้องเกินเกณฑ์ "ติดต่อกัน" ก่อนจะยิงแจ้งเตือนจริง
// ช่วยกรองสัญญาณชั่วคราว (เช่น กองไฟแคมป์ปิ้ง, แดดส่องเซนเซอร์ชั่วครู่) ออกจากไฟที่ลุกลามต่อเนื่อง
// ค่ายิ่งสูง ยิ่งกันแจ้งเตือนเท็จได้ดีขึ้น แต่จะแจ้งเตือนช้าลงตามจำนวนรอบที่ตั้งไว้ x ความถี่การส่งของ node
const ALERT_CONSEC_REQUIRED = Math.max(1, Number(process.env.ALERT_CONSEC_REQUIRED || 3));

// ---------- ควัน (MQ-2): เกณฑ์แบบปรับตาม baseline ของแต่ละ node เอง ----------
// เซ็นเซอร์แต่ละตัวมีค่า "อากาศปกติ" (ไม่มีควัน) ไม่เท่ากัน (เช่น node1 ~400-700, node2 ~1200-1600)
// แทนที่จะใช้ตัวเลขตายตัวตัวเดียวกันทั้ง 2 node เราให้ระบบ "เรียนรู้" ค่าฐาน (baseline) ของ
// แต่ละ node เองจากค่าที่อ่านได้ตอนไม่มีควัน แล้วแจ้งเตือนเมื่อค่าสูงกว่า baseline ของตัวเอง
// SMOKE_BASELINE_RATIO เท่า — node ไหน baseline สูง เกณฑ์ก็จะขยับสูงตามโดยอัตโนมัติ
const SMOKE_BASELINE_RATIO = Number(process.env.SMOKE_BASELINE_RATIO || 2.5);
// ต้องมีตัวอย่างค่า "ปกติ" อย่างน้อยเท่านี้ก่อน ถึงจะเริ่มเชื่อ baseline ที่เรียนรู้มา
// ก่อนหน้านั้นจะใช้ SMOKE_THRESHOLD_FALLBACK (ตัวเลขตายตัว) ไปพลางๆ เพื่อความปลอดภัย
const SMOKE_BASELINE_MIN_SAMPLES = Math.max(5, Number(process.env.SMOKE_BASELINE_MIN_SAMPLES || 30));
// หลังจากเชื่อ baseline แล้ว ให้มันขยับตามการเปลี่ยนแปลงช้าๆ ต่อไปเรื่อยๆ (เช่น เปลี่ยนเซนเซอร์ใหม่)
// ค่ายิ่งน้อย ยิ่งปรับตัวช้า (นิ่งกว่า); ค่ายิ่งมาก ยิ่งปรับตัวไว (แต่เสี่ยงไหลตามค่าที่ค่อยๆ สูงขึ้นผิดปกติ)
const SMOKE_BASELINE_ALPHA = Number(process.env.SMOKE_BASELINE_ALPHA || 0.02);
// เกณฑ์ตายตัวที่ใช้ชั่วคราวระหว่างที่ยังเก็บตัวอย่างไม่ครบ (เผื่อไว้ก่อนระบบรู้จัก baseline จริง)
const SMOKE_THRESHOLD_FALLBACK = Number(process.env.SMOKE_THRESHOLD_FALLBACK || process.env.SMOKE_THRESHOLD || 2000);
// เพดานความปลอดภัยสัมบูรณ์: ไม่ว่า baseline จะเรียนรู้ผิดเพี้ยนไปทางไหน ถ้าค่าดิบสูงทะลุเพดานนี้
// ให้ถือว่าผิดปกติทันทีเสมอ (กันกรณี baseline ถูกสอนให้ "ชิน" กับควันที่ค่อยๆ มากขึ้นช้าๆ)
const SMOKE_SAFETY_CEILING = Number(process.env.SMOKE_SAFETY_CEILING || 3500);

app.use(cors());
app.use(express.json({ limit: '100kb' }));
app.use(express.static(path.join(__dirname, '..', 'public')));

// Track alert state for each node
const wasAlerting = {
  node1: false,
  node2: false
};

// ---------- Health check ----------
app.get('/api/health', (req, res) => {
  res.json({
    ok: true,
    time: Date.now()
  });
});

// ---------- ESP32 data endpoint ----------
app.post('/firedata', (req, res) => {
  // Verify API key
  const got = req.header('x-api-key');

  if (!API_KEY || got !== API_KEY) {
    return res.status(401).json({
      ok: false,
      error: 'Unauthorized'
    });
  }

  const d = req.body || {};

  // Validate node ID
  if (!['node1', 'node2'].includes(d.nodeId)) {
    return res.status(400).json({
      ok: false,
      error: 'nodeId must be node1 or node2'
    });
  }

  // Validate sensor values
  const temperature = Number(d.temperature);
  const humidity = Number(d.humidity);
  const smoke_level = Number(d.smoke_level);

  if (![temperature, humidity, smoke_level].every(Number.isFinite)) {
    return res.status(400).json({
      ok: false,
      error: 'Invalid sensor values'
    });
  }

  // เกณฑ์ควันของ node นี้ "วันนี้" คืออะไร — คำนวณจาก baseline ที่เรียนรู้มา (หรือ fallback ถ้ายังเรียนรู้ไม่พอ)
  const smokeInfo = store.getEffectiveSmokeThreshold(d.nodeId, {
    ratio: SMOKE_BASELINE_RATIO,
    minSamples: SMOKE_BASELINE_MIN_SAMPLES,
    fallback: SMOKE_THRESHOLD_FALLBACK
  });

  // ค่าครั้งนี้เกินเกณฑ์หรือยัง (ยังไม่ตัดสินว่าเป็น "ไฟจริง" แค่ครั้งเดียว)
  const instantBreach =
    d.fire_detected === true ||
    temperature >= TEMP_THRESHOLD ||
    humidity <= HUMIDITY_THRESHOLD ||
    smoke_level >= smokeInfo.threshold ||
    smoke_level >= SMOKE_SAFETY_CEILING;

  // อัปเดต baseline เฉพาะตอนที่ค่า "นิ่ง/ปกติ" เท่านั้น — ถ้ากำลังเกินเกณฑ์อยู่ ไม่อัปเดต
  // ไม่งั้นไฟที่ค่อยๆ ลุกลามจะค่อยๆ "สอน" ให้ baseline เลื่อนตามควันไปด้วย
  if (!instantBreach) {
    store.updateSmokeBaseline(d.nodeId, smoke_level, SMOKE_BASELINE_MIN_SAMPLES, SMOKE_BASELINE_ALPHA);
  }

  // เช็กว่าเกินเกณฑ์ติดต่อกันกี่ครั้งแล้ว และค่ายังมีแนวโน้มเพิ่มขึ้น/ไม่ลดลงหรือไม่
  const trend = store.trackTrend(
    d.nodeId,
    instantBreach,
    temperature,
    smoke_level,
    ALERT_CONSEC_REQUIRED
  );

  // "alert" (แจ้งเตือนจริง) ต้องเกินเกณฑ์ติดต่อกันครบตามที่ตั้งไว้เท่านั้น
  const alert = trend.sustained;
  // "watch" = เริ่มเกินเกณฑ์แล้วแต่ยังไม่ครบจำนวนรอบ ถือเป็นสถานะเฝ้าระวัง ยังไม่ฟันธง
  const watch = instantBreach && !alert;

  // Save sensor reading
  const reading = store.saveReading(d.nodeId, {
    temperature,
    humidity,
    smoke_level,
    alert,
    watch,
    consecutiveBreaches: trend.consecutiveBreaches,
    requiredConsecutive: trend.required,
    smokeBaseline: smokeInfo.baseline,
    smokeThresholdUsed: smokeInfo.threshold,
    smokeBaselineActive: smokeInfo.usingBaseline,
    smokeBaselineSamples: smokeInfo.sampleCount
  });

  // Remember previous alert state
  const wasAlertingBefore = wasAlerting[d.nodeId];

  // Update alert state
  wasAlerting[d.nodeId] = alert;

  // Respond immediately to ESP32
  // Do not wait for Telegram to finish
  res.status(200).json({
    ok: true,
    nodeId: d.nodeId,
    alert,
    watch,
    consecutiveBreaches: trend.consecutiveBreaches,
    requiredConsecutive: trend.required,
    smokeBaseline: smokeInfo.baseline,
    smokeThresholdUsed: smokeInfo.threshold,
    smokeBaselineActive: smokeInfo.usingBaseline
  });

  // Send Telegram notification in the background
  if (alert && !wasAlertingBefore) {
    (async () => {
      try {
        const message = telegram.alertMessage(
          d.nodeId,
          reading,
          trend,
          smokeInfo
        );

        const result = await telegram.sendMessage(message);

        const trendNote = trend.rising === false
          ? ' (ค่าเริ่มลดลง อาจมีคนควบคุมอยู่ ยังควรตรวจสอบ)'
          : ' (ค่ายังเพิ่มขึ้นต่อเนื่อง)';

        store.addAlert({
          type: 'alert',
          nodeId: d.nodeId,
          message:
            `อุณหภูมิ ${temperature} °C / ` +
            `ความชื้น ${humidity}% / ` +
            `ควัน MQ-2 = ${smoke_level} (เกณฑ์ ${smokeInfo.threshold}${smokeInfo.usingBaseline ? `, baseline ${smokeInfo.baseline}` : ' ค่าเริ่มต้น'})` +
            ` · เกินเกณฑ์ติดต่อกัน ${trend.consecutiveBreaches} ครั้ง` +
            trendNote,
          status: result.ok ? 'ส่งแล้ว' : 'ล้มเหลว'
        });
      } catch (err) {
        console.error('Telegram notification error:', err);

        store.addAlert({
          type: 'alert',
          nodeId: d.nodeId,
          message: 'เกิดข้อผิดพลาดในการส่ง Telegram',
          status: 'ล้มเหลว'
        });
      }
    })();
  } else if (!alert && wasAlertingBefore) {
    store.addAlert({
      type: 'recovered',
      nodeId: d.nodeId,
      message: 'ค่ากลับสู่ภาวะปกติ',
      status: 'บันทึกแล้ว'
    });
  }
}); // สำคัญ: ปิด app.post('/firedata') ให้ครบ

// ---------- Dashboard status ----------
app.get('/api/status', (req, res) => {
  const node1 = store.getNodeStatus('node1');
  const node2 = store.getNodeStatus('node2');

  const anyAlert =
    node1.status === 'ALERT' ||
    node2.status === 'ALERT';
  const anyWatch =
    node1.status === 'WATCH' ||
    node2.status === 'WATCH';

  const anyOnline = node1.online || node2.online;

  const baselineInfo = (nodeId) => store.getEffectiveSmokeThreshold(nodeId, {
    ratio: SMOKE_BASELINE_RATIO,
    minSamples: SMOKE_BASELINE_MIN_SAMPLES,
    fallback: SMOKE_THRESHOLD_FALLBACK
  });

  res.json({
    time: Date.now(),
    fire: anyAlert ? 'ALERT' : (anyWatch ? 'WATCH' : 'NORMAL'),

    nodes: {
      node1,
      node2
    },

    baseline: {
      node1: baselineInfo('node1'),
      node2: baselineInfo('node2')
    },

    system: {
      node1Online: node1.online,
      node2Online: node2.online,
      espNow: anyOnline ? 'ทำงานปกติ' : 'ไม่มีสัญญาณ',
      wifi: anyOnline ? 'เชื่อมต่อ' : 'ขาดการเชื่อมต่อ',
      telegram: telegram.isConfigured()
        ? 'พร้อมใช้งาน'
        : 'ยังไม่ได้ตั้งค่า'
    }
  });
});

// ---------- Dashboard history ----------
app.get('/api/history', (req, res) => {
  const minutes = Math.min(
    Number(req.query.minutes) || 1440,
    48 * 60
  );

  const bucketMinutes =
    Number(req.query.bucket) ||
    Math.max(1, Math.round(minutes / 144));

  res.json(store.getChartSeries(minutes, bucketMinutes));
});

// ---------- Dashboard alerts ----------
app.get('/api/alerts', (req, res) => {
  const limit = Math.min(
    Number(req.query.limit) || 50,
    300
  );

  res.json(store.getAlerts(limit));
});

// ---------- Reset a node's learned smoke baseline ----------
// Useful right after physically swapping/cleaning a sensor, so the system
// doesn't keep comparing new readings against the old sensor's baseline
// while it slowly re-learns. Normal drift (ageing, dust) doesn't need this —
// the EMA in store.js already tracks that automatically.
app.post('/api/baseline/reset/:nodeId', (req, res) => {
  const { nodeId } = req.params;
  if (!['node1', 'node2'].includes(nodeId)) {
    return res.status(400).json({ ok: false, error: 'nodeId must be node1 or node2' });
  }
  store.resetBaseline(nodeId);
  store.addAlert({
    type: 'test',
    nodeId,
    message: 'รีเซ็ตค่า baseline ควันด้วยตนเอง (เริ่มเรียนรู้ใหม่)',
    status: 'บันทึกแล้ว'
  });
  res.json({ ok: true, nodeId });
});

// ---------- Test Telegram ----------
app.post('/api/test-telegram', async (req, res) => {
  try {
    const result = await telegram.sendMessage(
      '✅ ทดสอบการแจ้งเตือนจาก Forest Fire Detection Dashboard\n' +
      'ระบบแจ้งเตือนทำงานปกติ'
    );

    store.addAlert({
      type: 'test',
      nodeId: '-',
      message: 'ทดสอบส่งข้อความผ่าน Telegram',
      status: result.ok
        ? 'สำเร็จ'
        : 'ล้มเหลว: ' + (result.error || '')
    });

    res.json(result);
  } catch (err) {
    console.error('Test Telegram error:', err);

    res.status(500).json({
      ok: false,
      error: 'Telegram request failed'
    });
  }
});

// ---------- Start server ----------
app.listen(PORT, () => {
  console.log(
    `Forest Fire Dashboard server listening on port ${PORT}`
  );
});