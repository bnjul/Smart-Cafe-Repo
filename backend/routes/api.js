const express = require('express');
const multer = require('multer');
const mongoose = require('mongoose');
const path = require('path');
const fs = require('fs');

const SensorReading = require('../models/SensorReading');
const CustomerEvent = require('../models/CustomerEvent');
const DeviceStatus = require('../models/DeviceStatus');
const Photo = require('../models/Photo');
const Settings = require('../models/Settings');
const FridgeEvent = require('../models/FridgeEvent');
const fridge = require('../services/fridgeService');
const { publishAcSet, publishThreshold } = require('../mqtt/mqttClient');

const router = express.Router();

// Express 4 does not catch errors thrown inside async handlers - without this,
// a single database hiccup would raise an unhandled rejection and take the
// whole server (and every dashboard) down. Wrap every route handler so a failure
// becomes a logged 500 for that one request instead.
['get', 'post', 'delete'].forEach((method) => {
  const original = router[method].bind(router);
  router[method] = (routePath, ...handlers) =>
    original(
      routePath,
      ...handlers.map((handler) => (req, res, next) => {
        Promise.resolve()
          .then(() => handler(req, res, next))
          .catch((err) => {
            console.error(`[API] ${method.toUpperCase()} ${req.originalUrl} failed:`, err.message);
            if (!res.headersSent) res.status(500).json({ error: 'Internal server error' });
          });
      })
    );
});

// ---------- Multer setup for camera photo uploads ----------
const uploadsDir = path.join(__dirname, '..', 'uploads');
if (!fs.existsSync(uploadsDir)) fs.mkdirSync(uploadsDir, { recursive: true });

const storage = multer.diskStorage({
  destination: (req, file, cb) => cb(null, uploadsDir),
  filename: (req, file, cb) => {
    const ts = Date.now();
    const safeReason = (req.body.reason || 'EVT').replace(/[^a-zA-Z0-9]/g, '');
    const idx = req.body.index || '0';
    cb(null, `${ts}_${safeReason}_${idx}.jpg`);
  }
});
const upload = multer({ storage, limits: { fileSize: 8 * 1024 * 1024 } });

function requireUploadSecret(req, res, next) {
  const secret = req.headers['x-upload-secret'];
  if (!secret || secret !== process.env.CAMERA_UPLOAD_SECRET) {
    return res.status(401).json({ error: 'Invalid or missing upload secret' });
  }
  next();
}

// ---------- Node/system status ----------
router.get('/status', async (req, res) => {
  const nodes = await DeviceStatus.find({});
  const OFFLINE_MS = 20000;
  const now = Date.now();
  const result = nodes.map((n) => ({
    deviceKey: n.deviceKey,
    online: n.online && (now - new Date(n.lastSeen).getTime() < OFFLINE_MS),
    lastSeen: n.lastSeen,
    extra: n.extra
  }));
  const activeCount = result.filter((n) => n.online).length;
  res.json({ nodes: result, activeCount, totalNodes: 6 });
});

// ---------- Refrigerator node ----------
// door/power come back as UNKNOWN when the controller is offline or its
// data is stale - the dashboard must never assume OFF/CLOSED in that case.
router.get('/fridge/state', async (req, res) => {
  try {
    res.json(await fridge.getView());
  } catch (err) {
    console.error('[API] /fridge/state error:', err.message);
    res.status(500).json({ error: 'Failed to read fridge state' });
  }
});

router.get('/fridge/events', async (req, res) => {
  try {
    const limit = Math.min(parseInt(req.query.limit || '50', 10) || 50, 200);
    const events = await FridgeEvent.find({}).sort({ timestamp: -1 }).limit(limit);
    res.json(events);
  } catch (err) {
    console.error('[API] /fridge/events error:', err.message);
    res.status(500).json({ error: 'Failed to read fridge events' });
  }
});

// ---------- Sensors ----------
router.get('/sensors/latest', async (req, res) => {
  const latest = await SensorReading.findOne().sort({ timestamp: -1 });
  res.json(latest || {});
});

router.get('/sensors/history', async (req, res) => {
  const hours = parseInt(req.query.hours || '24', 10);
  const since = new Date(Date.now() - hours * 3600 * 1000);
  const readings = await SensorReading.find({ timestamp: { $gte: since } })
    .sort({ timestamp: 1 })
    .limit(2000);
  res.json(readings);
});

// ---------- Customers ----------
router.get('/customers/summary', async (req, res) => {
  const startOfDay = new Date();
  startOfDay.setHours(0, 0, 0, 0);

  const inCount = await CustomerEvent.countDocuments({ type: 'IN', timestamp: { $gte: startOfDay } });
  const outCount = await CustomerEvent.countDocuments({ type: 'OUT', timestamp: { $gte: startOfDay } });
  const inside = Math.max(0, inCount - outCount);

  res.json({ in: inCount, out: outCount, inside });
});

router.get('/customers/history', async (req, res) => {
  const limit = parseInt(req.query.limit || '100', 10);
  const events = await CustomerEvent.find({}).sort({ timestamp: -1 }).limit(limit);
  res.json(events);
});

// ---------- Photos ----------
router.get('/photos', async (req, res) => {
  const limit = Math.min(parseInt(req.query.limit || '100', 10) || 100, 200);
  const skip = Math.max(parseInt(req.query.skip || '0', 10) || 0, 0);
  const photos = await Photo.find({}).sort({ timestamp: -1 }).skip(skip).limit(limit);
  res.json(photos);
});

router.delete('/photos', async (req, res) => {
  const ids = Array.isArray(req.body.ids) ? [...new Set(req.body.ids)] : [];
  if (!ids.length || ids.length > 200 || ids.some((id) => !mongoose.Types.ObjectId.isValid(id))) {
    return res.status(400).json({ error: 'ids must contain between 1 and 200 valid photo IDs' });
  }

  const photos = await Photo.find({ _id: { $in: ids } });
  if (photos.length !== ids.length) {
    return res.status(404).json({ error: 'One or more selected photos were not found' });
  }

  await Photo.deleteMany({ _id: { $in: ids } });
  await Promise.all(photos.map(async (photo) => {
    const filePath = path.resolve(uploadsDir, path.basename(photo.filename));
    if (!filePath.startsWith(`${path.resolve(uploadsDir)}${path.sep}`)) {
      throw new Error(`Refusing to delete file outside uploads directory: ${photo.filename}`);
    }
    try {
      await fs.promises.unlink(filePath);
    } catch (err) {
      if (err.code !== 'ENOENT') throw err;
    }
  }));

  const io = req.app.get('io');
  io.emit('photos_deleted', { ids });
  res.json({ ok: true, deleted: ids.length });
});

// Called by the ESP32-CAM firmware directly (multipart/form-data)
router.post('/photos/upload', requireUploadSecret, upload.single('photo'), async (req, res) => {
  try {
    if (!req.file) return res.status(400).json({ error: 'No photo file received' });

    const publicUrl = process.env.PUBLIC_URL || 'http://localhost:3000';
    const fileUrl = `${publicUrl}/uploads/${req.file.filename}`;

    const photo = await Photo.create({
      filename: req.file.filename,
      url: fileUrl,
      reason: req.body.reason === 'OUT' ? 'OUT' : 'IN',
      eventId: req.body.event_id || '',
      index: parseInt(req.body.index || '0', 10),
      device: req.body.device || 'camera01'
    });

    const io = req.app.get('io');
    io.emit('new_photo', photo);

    res.json({ ok: true, url: fileUrl });
  } catch (err) {
    console.error('[UPLOAD] Error saving photo:', err.message);
    res.status(500).json({ error: 'Failed to save photo' });
  }
});

// ---------- Settings (temperature threshold) ----------
router.get('/settings/threshold', async (req, res) => {
  const settings = await Settings.getSettings();
  res.json({ onTemp: settings.onTemp, offTemp: settings.offTemp });
});

router.post('/settings/threshold', async (req, res) => {
  const { onTemp, offTemp } = req.body;
  if (typeof onTemp !== 'number' || typeof offTemp !== 'number') {
    return res.status(400).json({ error: 'onTemp and offTemp must be numbers' });
  }
  if (offTemp >= onTemp) {
    return res.status(400).json({ error: 'offTemp must be lower than onTemp' });
  }

  const settings = await Settings.getSettings();
  settings.onTemp = onTemp;
  settings.offTemp = offTemp;
  await settings.save();

  publishThreshold(onTemp, offTemp); // push to hub via MQTT (retained)

  const io = req.app.get('io');
  io.emit('threshold_update', { onTemp, offTemp });

  res.json({ ok: true, onTemp, offTemp });
});

// ---------- Manual AC override ----------
router.post('/control/ac', async (req, res) => {
  const { state } = req.body;
  if (state !== 'ON' && state !== 'OFF') {
    return res.status(400).json({ error: 'state must be "ON" or "OFF"' });
  }
  publishAcSet(state);
  res.json({ ok: true, state });
});

module.exports = router;
