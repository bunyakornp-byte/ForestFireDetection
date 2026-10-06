// server/store.js
// Simple JSON-file-backed store (lowdb v1 / CommonJS).
// Holds: latest reading per node, rolling history (for charts) and alert log.
//
// NOTE on Render free plan: the local filesystem is NOT guaranteed to persist
// across deploys/restarts unless you attach a paid persistent disk. This is
// fine for a live-monitoring dashboard (status + recent history) but do not
// rely on it as a long-term database. See README for options.

const path = require('path');
const fs = require('fs');
const low = require('lowdb');
const FileSync = require('lowdb/adapters/FileSync');

const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, '..', 'data');
if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });

const adapter = new FileSync(path.join(DATA_DIR, 'app-data.json'));
const db = low(adapter);

db.defaults({
  latest: {},     // { node1: {...}, node2: {...} }
  history: [],    // [{ t, nodeId, temperature, humidity, smoke_level, alert }]
  alerts: [],     // [{ t, type, nodeId, message, status }]
  baseline: {}    // { node1: { mean, count, updatedAt }, node2: {...} } — learned "quiet" smoke level per node
}).write();

const HISTORY_RETENTION_MS = 48 * 60 * 60 * 1000; // keep 48h of raw readings
const HISTORY_MAX_POINTS = 20000;                 // hard cap, just in case
const ALERTS_MAX = 300;
const OFFLINE_MS = 15000; // a node with no message in 15s is considered offline

// ---------- Trend tracking (in-memory only, resets on restart — that's fine) ----------
// Goal: tell apart a brief spike (someone's campfire, a passing heat gust, sun on the
// MQ-2 for a moment) from a real, escalating fire: a real fire keeps breaching the
// threshold for several readings IN A ROW and the values don't drop back down.
const TREND_WINDOW_MAX = 8; // keep a bit more history than the usual consecRequired
const trendBuffers = { node1: [], node2: [] };

function trackTrend(nodeId, breach, temperature, smoke_level, consecRequired = 3) {
  const buf = trendBuffers[nodeId] || (trendBuffers[nodeId] = []);
  buf.push({ breach: !!breach, temperature, smoke_level });
  if (buf.length > TREND_WINDOW_MAX) buf.shift();

  // How many readings in a row (counting back from the newest) are over threshold.
  let consecutiveBreaches = 0;
  for (let i = buf.length - 1; i >= 0; i--) {
    if (buf[i].breach) consecutiveBreaches++;
    else break;
  }

  const sustained = consecutiveBreaches >= consecRequired;

  // Over that same run, is it still climbing (or at least not coming back down)?
  let rising = null;
  if (consecutiveBreaches >= 2) {
    const run = buf.slice(buf.length - consecutiveBreaches);
    const first = run[0];
    const last = run[run.length - 1];
    rising = last.smoke_level >= first.smoke_level || last.temperature >= first.temperature;
  }

  return { consecutiveBreaches, sustained, rising, required: consecRequired };
}

function resetTrend(nodeId) {
  trendBuffers[nodeId] = [];
}

// ---------- Adaptive per-node smoke baseline ----------
// Different MQ-2 units (and different mounting/airflow) sit at very different
// "clean air" ADC readings — e.g. ~400-700 on one board, ~1200-1600 on another.
// A single fixed threshold for both is either too sensitive on one node or
// deaf on the other. Instead we learn each node's own quiet-air baseline over
// time and alert when a node reads far ABOVE its *own* baseline, not above
// some number picked for a different sensor.
//
// Learning rule: every reading that is NOT currently flagged as a breach
// nudges that node's baseline. Early on (count <= minSamples) we use a plain
// running average so it converges fast; after that we switch to a slow EMA
// so the baseline keeps drifting with the sensor (dust build-up, ageing,
// swapped sensor) without being thrown off by any single reading. Readings
// taken *during* a breach never update the baseline — otherwise a real fire
// would slowly "teach" the system that smoke is normal.

function getBaseline(nodeId) {
  return db.get(`baseline.${nodeId}`).value() || null;
}

function updateSmokeBaseline(nodeId, smokeLevel, minSamples = 30, alpha = 0.02) {
  const current = getBaseline(nodeId);
  const count = (current ? current.count : 0) + 1;
  let mean;
  if (!current) {
    mean = smokeLevel;
  } else if (count <= minSamples) {
    // plain running average while warming up — converges quickly from scratch
    mean = current.mean + (smokeLevel - current.mean) / count;
  } else {
    // slow exponential drift once we trust the baseline, so it keeps adapting
    // (e.g. a replaced sensor) without reacting hard to any one reading
    mean = current.mean + alpha * (smokeLevel - current.mean);
  }
  const entry = { mean: +mean.toFixed(1), count, updatedAt: now() };
  db.set(`baseline.${nodeId}`, entry).write();
  return entry;
}

function resetBaseline(nodeId) {
  db.unset(`baseline.${nodeId}`).write();
  resetTrend(nodeId);
}

// Returns the smoke ADC value that should trigger a breach for this node right now.
// Falls back to a fixed absolute threshold until the node has enough "quiet"
// samples to trust its own learned baseline.
function getEffectiveSmokeThreshold(nodeId, { ratio = 2.5, minSamples = 30, fallback = 2000 } = {}) {
  const b = getBaseline(nodeId);
  if (!b || b.count < minSamples) {
    return {
      threshold: fallback,
      usingBaseline: false,
      baseline: b ? b.mean : null,
      sampleCount: b ? b.count : 0,
      minSamples
    };
  }
  return {
    threshold: +(b.mean * ratio).toFixed(1),
    usingBaseline: true,
    baseline: b.mean,
    sampleCount: b.count,
    minSamples
  };
}

function now() {
  return Date.now();
}

function getLatest() {
  return db.get('latest').value() || {};
}

function getNodeStatus(nodeId) {
  const latest = getLatest()[nodeId];
  if (!latest) return { nodeId, online: false, status: 'รอข้อมูล' };
  const online = now() - latest.t <= OFFLINE_MS;
  let status = 'NORMAL';
  if (!online) status = 'OFFLINE';
  else if (latest.alert) status = 'ALERT';
  else if (latest.watch) status = 'WATCH';
  return { ...latest, nodeId, online, status };
}

function saveReading(nodeId, reading) {
  const entry = {
    t: now(),
    nodeId,
    temperature: reading.temperature,
    humidity: reading.humidity,
    smoke_level: reading.smoke_level,
    alert: !!reading.alert,
    watch: !!reading.watch,
    consecutiveBreaches: reading.consecutiveBreaches || 0,
    requiredConsecutive: reading.requiredConsecutive || 0,
    smokeBaseline: reading.smokeBaseline ?? null,
    smokeThresholdUsed: reading.smokeThresholdUsed ?? null,
    smokeBaselineActive: !!reading.smokeBaselineActive,
    smokeBaselineSamples: reading.smokeBaselineSamples || 0
  };

  db.set(`latest.${nodeId}`, entry).write();
  db.get('history').push(entry).write();

  // prune old history occasionally (cheap check every write; fine at this scale)
  const cutoff = now() - HISTORY_RETENTION_MS;
  const hist = db.get('history').value();
  if (hist.length > HISTORY_MAX_POINTS || (hist[0] && hist[0].t < cutoff)) {
    const pruned = hist.filter(h => h.t >= cutoff).slice(-HISTORY_MAX_POINTS);
    db.set('history', pruned).write();
  }

  return entry;
}

function getHistory(minutes = 1440) {
  const cutoff = now() - minutes * 60 * 1000;
  return db.get('history').filter(h => h.t >= cutoff).value();
}

// Downsample history into evenly spaced buckets per node, good for line charts.
function getChartSeries(minutes = 1440, bucketMinutes = 10) {
  const raw = getHistory(minutes);
  const bucketMs = bucketMinutes * 60 * 1000;
  const buckets = {}; // bucketStart -> { node1: {tSum,tCount,hSum,hCount}, node2: {...} }

  raw.forEach(r => {
    const bucketStart = Math.floor(r.t / bucketMs) * bucketMs;
    if (!buckets[bucketStart]) buckets[bucketStart] = {};
    if (!buckets[bucketStart][r.nodeId]) {
      buckets[bucketStart][r.nodeId] = { tSum: 0, tCount: 0, hSum: 0, hCount: 0 };
    }
    const b = buckets[bucketStart][r.nodeId];
    b.tSum += r.temperature; b.tCount += 1;
    b.hSum += r.humidity; b.hCount += 1;
  });

  const labels = Object.keys(buckets).map(Number).sort((a, b) => a - b);
  const series = { labels: [], node1: { temp: [], hum: [] }, node2: { temp: [], hum: [] } };

  labels.forEach(ts => {
    series.labels.push(ts);
    ['node1', 'node2'].forEach(nodeId => {
      const b = buckets[ts][nodeId];
      series[nodeId].temp.push(b ? +(b.tSum / b.tCount).toFixed(1) : null);
      series[nodeId].hum.push(b ? +(b.hSum / b.hCount).toFixed(1) : null);
    });
  });

  return series;
}

function addAlert(entry) {
  const row = { t: now(), ...entry };
  db.get('alerts').push(row).write();
  const alerts = db.get('alerts').value();
  if (alerts.length > ALERTS_MAX) {
    db.set('alerts', alerts.slice(-ALERTS_MAX)).write();
  }
  return row;
}

function getAlerts(limit = 50) {
  const alerts = db.get('alerts').value() || [];
  return alerts.slice(-limit).reverse();
}

module.exports = {
  OFFLINE_MS,
  getLatest,
  getNodeStatus,
  saveReading,
  getHistory,
  getChartSeries,
  addAlert,
  getAlerts,
  trackTrend,
  resetTrend,
  getBaseline,
  updateSmokeBaseline,
  resetBaseline,
  getEffectiveSmokeThreshold
};
