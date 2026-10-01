const STATUS_POLL_MS = 4000;
const HISTORY_POLL_MS = 60000;
const ALERTS_POLL_MS = 10000;

function fmt(n, suffix = '') {
  return (n === null || n === undefined || Number.isNaN(n)) ? '--' : `${n}${suffix}`;
}

function setStatusChip(el, online) {
  el.textContent = online ? '● ออนไลน์' : '● ออฟไลน์';
  el.classList.toggle('online', online);
}

function updateClock() {
  const el = document.getElementById('clock');
  const now = new Date();
  const dateStr = now.toLocaleDateString('th-TH', { day: '2-digit', month: 'short', year: 'numeric' });
  const timeStr = now.toLocaleTimeString('th-TH', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  el.textContent = `${dateStr}  ${timeStr}`;
}

async function fetchJSON(url, opts) {
  const res = await fetch(url, opts);
  if (!res.ok) throw new Error('HTTP ' + res.status);
  return res.json();
}

function renderNode(nodeId, node) {
  document.getElementById(`${nodeId}-temp`).textContent = fmt(node.temperature, ' °C');
  document.getElementById(`${nodeId}-hum`).textContent = fmt(node.humidity, ' %');
  document.getElementById(`${nodeId}-smoke`).textContent = fmt(node.smoke_level);
  setStatusChip(document.getElementById(`${nodeId}-online`), node.online);

  const card = document.getElementById(`card-node${nodeId.slice(-1)}`);
  card.classList.toggle('is-alert', node.status === 'ALERT');

  const summary = document.getElementById(`summary-${nodeId}`);
  const rows = summary.querySelectorAll('.summary-row span');
  rows[0].textContent = fmt(node.temperature, ' °C');
  rows[1].textContent = fmt(node.humidity, ' %');
  rows[2].textContent = fmt(node.smoke_level);
}

function sysIcon(ok, warnOk) {
  if (warnOk === 'warn') return { text: '●', cls: 'warn' };
  return ok ? { text: '●', cls: '' } : { text: '●', cls: 'off' };
}

async function pollStatus() {
  try {
    const data = await fetchJSON('/api/status');

    renderNode('node1', data.nodes.node1);
    renderNode('node2', data.nodes.node2);

    const sysPill = document.getElementById('sys-pill');
    const fireState = document.getElementById('fire-state');
    const fireDesc = document.getElementById('fire-desc');
    const fireCard = document.getElementById('fire-card');

    if (data.fire === 'ALERT') {
      sysPill.textContent = '● ตรวจพบความผิดปกติ';
      sysPill.className = 'pill pill-alert';
      fireState.textContent = 'เตือนภัย';
      fireState.className = 'fire-state alert';
      fireDesc.textContent = 'ตรวจพบค่าผิดปกติ กรุณาตรวจสอบพื้นที่โดยด่วน';
      fireCard.classList.add('is-alert');
    } else {
      sysPill.textContent = '● ระบบทำงานปกติ';
      sysPill.className = 'pill pill-ok';
      fireState.textContent = 'ปกติ';
      fireState.className = 'fire-state';
      fireDesc.textContent = 'ไม่มีความเสี่ยงไฟไหม้ · ระบบตรวจสอบตามปกติ';
      fireCard.classList.remove('is-alert');
    }

    const n1 = sysIcon(data.system.node1Online);
    const n2 = sysIcon(data.system.node2Online);
    const wifi = sysIcon(data.system.wifi === 'เชื่อมต่อ');
    const espnow = sysIcon(data.system.espNow === 'ทำงานปกติ');
    const tg = sysIcon(data.system.telegram === 'พร้อมใช้งาน');

    const setSys = (id, s) => {
      const el = document.getElementById(id);
      el.className = 'sys-val' + (s.cls ? ' ' + s.cls : '');
    };
    setSys('sys-node1', n1);
    setSys('sys-node2', n2);
    setSys('sys-wifi', wifi);
    setSys('sys-espnow', espnow);
    setSys('sys-telegram', tg);

    const tgPill = document.getElementById('tg-pill');
    const tgStatus = document.getElementById('tg-status');
    if (data.system.telegram === 'พร้อมใช้งาน') {
      tgPill.textContent = '📨 Telegram พร้อมใช้งาน';
      tgStatus.textContent = '✅ เชื่อมต่อสำเร็จ: Bot พร้อมส่งการแจ้งเตือน';
      tgStatus.classList.remove('off');
    } else {
      tgPill.textContent = '📨 Telegram ยังไม่ตั้งค่า';
      tgStatus.textContent = '⚠️ ยังไม่ได้ตั้งค่า TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID';
      tgStatus.classList.add('off');
    }
  } catch (err) {
    console.error('status poll failed', err);
  }
}

let tempChart, humChart;

function buildCharts() {
  const ctxT = document.getElementById('chartTemp').getContext('2d');
  const ctxH = document.getElementById('chartHum').getContext('2d');

  const common = {
    type: 'line',
    data: { labels: [], datasets: [] },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { intersect: false, mode: 'index' },
      scales: {
        x: { ticks: { color: '#8ca0bc', maxTicksLimit: 8 }, grid: { color: 'rgba(255,255,255,.05)' } },
        y: { ticks: { color: '#8ca0bc' }, grid: { color: 'rgba(255,255,255,.05)' } }
      },
      plugins: { legend: { labels: { color: '#e7eef7', boxWidth: 12, font: { size: 11 } } } }
    }
  };

  tempChart = new Chart(ctxT, JSON.parse(JSON.stringify(common)));
  humChart = new Chart(ctxH, JSON.parse(JSON.stringify(common)));
}

function tsToLabel(ts) {
  return new Date(ts).toLocaleTimeString('th-TH', { hour: '2-digit', minute: '2-digit' });
}

async function pollHistory() {
  try {
    const data = await fetchJSON('/api/history?minutes=1440');
    const labels = data.labels.map(tsToLabel);

    tempChart.data.labels = labels;
    tempChart.data.datasets = [
      { label: 'อุณหภูมิ (Node 1)', data: data.node1.temp, borderColor: '#ffb648', backgroundColor: 'transparent', tension: 0.35, pointRadius: 0 },
      { label: 'อุณหภูมิ (Node 2)', data: data.node2.temp, borderColor: '#3b9cff', backgroundColor: 'transparent', tension: 0.35, pointRadius: 0 }
    ];
    tempChart.update();

    humChart.data.labels = labels;
    humChart.data.datasets = [
      { label: 'ความชื้น (Node 1)', data: data.node1.hum, borderColor: '#2fd97a', backgroundColor: 'transparent', tension: 0.35, pointRadius: 0 },
      { label: 'ความชื้น (Node 2)', data: data.node2.hum, borderColor: '#b98bff', backgroundColor: 'transparent', tension: 0.35, pointRadius: 0 }
    ];
    humChart.update();
  } catch (err) {
    console.error('history poll failed', err);
  }
}

function alertBadge(type) {
  if (type === 'alert') return '<span class="badge badge-alert">แจ้งเตือน</span>';
  if (type === 'recovered') return '<span class="badge badge-ok">กลับสู่ปกติ</span>';
  return '<span class="badge badge-warn">ทดสอบ</span>';
}

async function pollAlerts() {
  try {
    const rows = await fetchJSON('/api/alerts?limit=20');
    const body = document.getElementById('alerts-body');
    if (!rows.length) {
      body.innerHTML = '<tr><td colspan="4" class="empty">ยังไม่มีประวัติการแจ้งเตือน</td></tr>';
      return;
    }
    body.innerHTML = rows.map(r => `
      <tr>
        <td>${new Date(r.t).toLocaleString('th-TH', { dateStyle: 'short', timeStyle: 'medium' })}</td>
        <td>${alertBadge(r.type)}</td>
        <td>${r.nodeId !== '-' ? `[${r.nodeId}] ` : ''}${r.message}</td>
        <td>${r.status}</td>
      </tr>`).join('');
  } catch (err) {
    console.error('alerts poll failed', err);
  }
}

document.getElementById('tg-test-btn').addEventListener('click', async () => {
  const btn = document.getElementById('tg-test-btn');
  const resultEl = document.getElementById('tg-result');
  btn.disabled = true;
  resultEl.textContent = 'กำลังส่งข้อความทดสอบ...';
  try {
    const result = await fetchJSON('/api/test-telegram', { method: 'POST' });
    resultEl.textContent = result.ok ? '✅ ส่งข้อความทดสอบสำเร็จ' : `❌ ส่งไม่สำเร็จ: ${result.error || ''}`;
    pollAlerts();
  } catch (err) {
    resultEl.textContent = '❌ เกิดข้อผิดพลาด: ' + err.message;
  } finally {
    btn.disabled = false;
  }
});

// Sidebar tabs are placeholders for now; the whole dashboard lives on one page.
document.querySelectorAll('.nav-item').forEach(item => {
  item.addEventListener('click', e => {
    e.preventDefault();
    document.querySelectorAll('.nav-item').forEach(i => i.classList.remove('active'));
    item.classList.add('active');
    const target = item.dataset.tab === 'alerts' ? '.alerts-card'
      : item.dataset.tab === 'sensors' ? '.grid-mid'
      : item.dataset.tab === 'settings' ? '.sys-card'
      : '.grid-top';
    document.querySelector(target)?.scrollIntoView({ behavior: 'smooth', block: 'start' });
  });
});

buildCharts();
updateClock();
pollStatus();
pollHistory();
pollAlerts();

setInterval(updateClock, 1000);
setInterval(pollStatus, STATUS_POLL_MS);
setInterval(pollHistory, HISTORY_POLL_MS);
setInterval(pollAlerts, ALERTS_POLL_MS);
