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
  alerts: []      // [{ t, type, nodeId, message, status }]
}).write();

const HISTORY_RETENTION_MS = 48 * 60 * 60 * 1000; // keep 48h of raw readings
const HISTORY_MAX_POINTS = 20000;                 // hard cap, just in case
const ALERTS_MAX = 300;
const OFFLINE_MS = 15000; // a node with no message in 15s is considered offline

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
  return { ...latest, nodeId, online, status };
}

function saveReading(nodeId, reading) {
  const entry = {
    t: now(),
    nodeId,
    temperature: reading.temperature,
    humidity: reading.humidity,
    smoke_level: reading.smoke_level,
    alert: !!reading.alert
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
  getAlerts
};
