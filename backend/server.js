require('dotenv').config();

const express = require('express');
const http = require('http');
const path = require('path');
const cors = require('cors');
const { Server } = require('socket.io');

const connectDB = require('./config/db');
const { initMqtt, publishThreshold } = require('./mqtt/mqttClient');
const Settings = require('./models/Settings');
const DeviceStatus = require('./models/DeviceStatus');
const fridgeService = require('./services/fridgeService');
const apiRoutes = require('./routes/api');

const app = express();
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: '*' } });

app.set('io', io);
app.use(cors());
app.use(express.json());
app.use('/uploads', express.static(path.join(__dirname, 'uploads')));
app.use(express.static(path.join(__dirname, 'public')));
app.use('/api', apiRoutes);

io.on('connection', (socket) => {
  console.log(`[SOCKET] Dashboard client connected: ${socket.id}`);
  socket.on('disconnect', () => console.log(`[SOCKET] Client disconnected: ${socket.id}`));
});

// Periodically flip stale nodes to "offline" in the DB and notify dashboards.
// (We only get "online" pushes from MQTT status/data messages - this loop
// is what actually detects a node going silent.)
const OFFLINE_TIMEOUT_MS = 20000;
setInterval(async () => {
  try {
    const cutoff = new Date(Date.now() - OFFLINE_TIMEOUT_MS);
    const stale = await DeviceStatus.find({ online: true, lastSeen: { $lt: cutoff } });
    for (const node of stale) {
      // Atomic flip: if the Last Will handler already marked it offline, skip (no duplicate events)
      const flipped = await DeviceStatus.findOneAndUpdate(
        { _id: node._id, online: true },
        { $set: { online: false } }
      );
      if (!flipped) continue;
      io.emit('node_status', { deviceKey: node.deviceKey, online: false });
      console.log(`[WATCHDOG] Node "${node.deviceKey}" marked offline (no message in ${OFFLINE_TIMEOUT_MS / 1000}s)`);

      if (node.deviceKey === 'fridge') {
        await fridgeService.logEvent(io, 'CONTROLLER_OFFLINE', `No heartbeat for ${OFFLINE_TIMEOUT_MS / 1000}s`);
        io.emit('fridge_update', await fridgeService.getView()); // dashboards show UNKNOWN
      }
    }
  } catch (err) {
    console.error('[WATCHDOG] Error:', err.message);
  }
}, 5000);

async function start() {
  await connectDB();
  initMqtt(io);

  // Re-assert the saved threshold on startup (retained MQTT message),
  // so the hub always has the latest dashboard-configured values.
  const settings = await Settings.getSettings();
  setTimeout(() => {
    try {
      publishThreshold(settings.onTemp, settings.offTemp);
      console.log(`[STARTUP] Published threshold ON=${settings.onTemp} OFF=${settings.offTemp}`);
    } catch (err) {
      console.error('[STARTUP] Could not publish initial threshold:', err.message);
    }
  }, 2000); // small delay to let MQTT client finish connecting

  const PORT = process.env.PORT || 3000;
  server.listen(PORT, () => {
    console.log(`[SERVER] Cafe IoT dashboard running at http://localhost:${PORT}`);
  });
}

start();
