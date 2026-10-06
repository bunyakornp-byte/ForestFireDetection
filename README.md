# Forest Fire Detection Dashboard (ESP32 + Node.js + Render)

Cloud-hosted dashboard for a 2-node forest-fire detection system:
`ESP32 Slave1 / Slave2 --(ESP-NOW)--> ESP32 Master --(HTTPS POST)--> Node.js/Express app (Render) --> Web dashboard + Telegram alerts`

No Node-RED and no local server required — the whole backend + dashboard is one small Node.js app you deploy to Render for free.

## What's included
- `server/` — Express API: receives sensor data, stores recent history, fires Telegram alerts, serves the dashboard.
- `public/` — the dashboard itself (HTML/CSS/JS, dark theme, 2 node cards, 24h charts, alert history, Telegram card, system status).
- `firmware/` — the three Arduino sketches (`master`, `slave1`, `slave2`), unchanged except `master.ino` now points at your Render URL instead of a local Node-RED instance.
- `render.yaml` — one-click Render Blueprint config.

## Dashboard features
- Live cards for Node 1 / Node 2: temperature, humidity, smoke (MQ-2), online/offline status
- Overall fire status banner (ปกติ / เตือนภัย)
- 24-hour history charts for temperature and humidity (both nodes)
- Sensor summary panel with latest readings
- Alert history table (alerts, recoveries, manual tests)
- Telegram status card + "ทดสอบการแจ้งเตือน" test button
- System status panel (ESP32 node 1/2, Wi-Fi, ESP-NOW, Telegram)
- Public, read-only dashboard — no login required
- Auto-refreshes every few seconds (polling, no extra setup needed)

## 1. Run locally (optional, to test before deploying)
```bash
cp .env.example .env
# edit .env: set API_KEY, TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID
npm install
npm start
```
Open `http://localhost:10000`.

Send a test reading:
```bash
curl -X POST http://localhost:10000/firedata \
  -H "Content-Type: application/json" \
  -H "x-api-key: YOUR_API_KEY" \
  -d '{"nodeId":"node1","temperature":27.5,"humidity":55,"smoke_level":120,"fire_detected":false}'
```

## 2. Put the project on GitHub
```bash
git init
git add .
git commit -m "Forest fire detection dashboard"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/forest-fire-dashboard.git
git push -u origin main
```
`.env` is already in `.gitignore` — never commit real secrets.

## 3. Deploy to Render
**Option A — Blueprint (uses `render.yaml`, recommended)**
1. Go to the Render dashboard → **New** → **Blueprint**.
2. Connect your GitHub repo.
3. Render reads `render.yaml` and creates the web service automatically.
4. When prompted, fill in the environment variables: `API_KEY`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`.
5. Click **Apply** / **Deploy**.

**Option B — Manual web service**
1. Render dashboard → **New** → **Web Service** → connect your repo.
2. Runtime: **Node**. Build command: `npm install`. Start command: `npm start`.
3. Under **Environment**, add:
   - `API_KEY` — a long random string (also used by the ESP32 Master)
   - `TELEGRAM_BOT_TOKEN` — from @BotFather
   - `TELEGRAM_CHAT_ID` — your chat/group id
   - optionally `TEMP_THRESHOLD`, `HUMIDITY_THRESHOLD`, `SMOKE_THRESHOLD`
4. Deploy. Render gives you a URL like `https://forest-fire-dashboard.onrender.com`.

Open that URL — you should see the dashboard (showing "รอข้อมูล" until the ESP32 sends data).

> **Free-tier note:** Render's free web services spin down after inactivity and spin back up on the next request (can take ~30–60s), and the local filesystem is not guaranteed to persist across restarts/deploys. That's fine for live monitoring + recent history, but don't treat it as a permanent database. If you need guaranteed uptime or long-term storage, upgrade the Render plan or add a managed database later — the code only touches `server/store.js`, so swapping storage later is isolated to that one file.

## 4. Point the ESP32 Master at your Render URL
In `firmware/master/master.ino`:
```cpp
const char* NODE_RED_URL = "https://forest-fire-dashboard.onrender.com/firedata";
const char* API_KEY = "THE_SAME_API_KEY_YOU_SET_ON_RENDER";
```
Also set your real Wi-Fi credentials. The Master now needs **real internet access** (a router or a phone hotspot with mobile data), not just a local network, since it posts straight to Render over HTTPS. Re-upload the sketch.

`slave1.ino` and `slave2.ino` are unchanged — they still only need to reach the Master over ESP-NOW on the same Wi-Fi channel.

## 5. Telegram setup
1. Create a bot via `@BotFather`, copy the token into `TELEGRAM_BOT_TOKEN`.
2. Get your chat id (e.g. message `@userinfobot` or your bot once and check `https://api.telegram.org/bot<token>/getUpdates`), set `TELEGRAM_CHAT_ID`.
3. Start a chat with your bot (or add it to the target group) so it's allowed to message you.
4. Use the **"ทดสอบการแจ้งเตือน"** button on the dashboard to confirm it works.

## 6. Thresholds

**Temperature / humidity** use fixed thresholds (override via env vars): temperature ≥ `TEMP_THRESHOLD` (default 50 °C), humidity ≤ `HUMIDITY_THRESHOLD` (default 20%). These are demo values — calibrate them for your real environment.

**Smoke (MQ-2) uses an adaptive per-node baseline instead of a fixed number.** Different MQ-2 units — even the same model — rest at very different "clean air" ADC readings (e.g. ~400-700 on one board, ~1200-1600 on another) depending on manufacturing tolerance, enclosure, and airflow. A single fixed number for both nodes is either too sensitive on one or deaf on the other, so instead:

1. Every time a node reports a reading that is **not** currently flagged as a breach, the server nudges that node's learned "quiet air" baseline (`server/store.js` → `updateSmokeBaseline`). Early readings (first `SMOKE_BASELINE_MIN_SAMPLES`, default 30) average together quickly; after that it drifts slowly (`SMOKE_BASELINE_ALPHA`, default 0.02) so it keeps tracking the sensor over time — ageing, dust, or a swapped sensor — without overreacting to any single reading.
2. The live alert threshold for that node is `baseline × SMOKE_BASELINE_RATIO` (default 2.5×). Node1 and Node2 end up with different absolute thresholds automatically, matching each sensor's own normal range.
3. Readings taken **during** a breach never update the baseline — otherwise a slow-building fire would gradually "teach" the system that smoke is normal.
4. Before a node has collected `SMOKE_BASELINE_MIN_SAMPLES` quiet readings (e.g. right after first boot), it falls back to a fixed `SMOKE_THRESHOLD_FALLBACK` (default 2000) so it's never left without any threshold.
5. `SMOKE_SAFETY_CEILING` (default 3500) is an absolute hard ceiling: no matter what the learned baseline says, a raw reading above this always counts as a breach. This is a safety net in case a baseline ever drifts somewhere it shouldn't.

The dashboard's **"เกณฑ์แจ้งเตือนควันแบบปรับอัตโนมัติ (Baseline)"** card shows each node's current learned baseline, live threshold, and whether it's still warming up — plus a **reset button per node**, which you should press right after physically replacing or cleaning a sensor so it stops comparing new readings against the old sensor's baseline while it re-learns. You don't need to reset anything for normal gradual drift — the EMA already follows that on its own.

Tune `SMOKE_BASELINE_RATIO` up if you're seeing false alarms from normal ambient variation, or down if real smoke isn't triggering an alert quickly enough. A threshold alert (of any kind) is not proof of an actual fire — it's a first-pass signal to go check.

A node is marked **offline** if no reading has arrived in the last 15 seconds.
