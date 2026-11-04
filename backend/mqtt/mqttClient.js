const mqtt = require('mqtt');

const SensorReading = require('../models/SensorReading');
const CustomerEvent = require('../models/CustomerEvent');
const DeviceStatus = require('../models/DeviceStatus');
const Settings = require('../models/Settings');
const fridge = require('../services/fridgeService');

// ---- Topic constants (must match firmware) ----
const T = {
  HUB_STATUS: 'cafe/hub/status',
  HUB_AGGREGATE: 'cafe/hub/aggregate',
  DOOR_STATUS: 'cafe/door/status',
  DOOR_EVENT: 'cafe/door/event',
  ENV_STATUS: 'cafe/env/status',
  ENV_DATA: 'cafe/env/data',
  CONTROL_STATUS: 'cafe/control/status',
  AC_STATE: 'cafe/control/ac/state',
  AC_SET: 'cafe/control/ac/set',
  CAMERA_STATUS: 'cafe/camera/status',
  CAMERA_CAPTURE: 'cafe/camera/capture',
  THRESHOLD_SET: 'cafe/config/threshold/set',
  FRIDGE_DOOR: 'cafe/fridge/door',
  FRIDGE_POWER: 'cafe/fridge/power',
  FRIDGE_TEMPERATURE: 'cafe/fridge/temperature',
  FRIDGE_HUMIDITY: 'cafe/fridge/humidity',
  FRIDGE_SENSOR: 'cafe/fridge/sensor',
  FRIDGE_AVAIL: 'cafe/fridge/availability',
  FRIDGE_TELEMETRY: 'cafe/fridge/telemetry',
  FRIDGE_ALERT: 'cafe/fridge/alert',
  ALL: 'cafe/#'
};

// Alert strings the fridge node is allowed to publish on cafe/fridge/alert
const FRIDGE_NODE_ALERTS = [
  'DOOR_OPEN_WARNING', 'SUPPLY_LOST', 'SUPPLY_RESTORED',
  'TEMP_HIGH', 'TEMP_NORMAL', 'SENSOR_FAULT', 'SENSOR_OK'
];

let client = null;

function deviceKeyFromStatusTopic(topic) {
  if (topic === T.HUB_STATUS) return 'hub';
  if (topic === T.DOOR_STATUS) return 'door';
  if (topic === T.ENV_STATUS) return 'env';
  if (topic === T.CONTROL_STATUS) return 'control';
  if (topic === T.CAMERA_STATUS) return 'camera';
  if (topic === T.FRIDGE_AVAIL) return 'fridge';
  return null;
}

async function markOnline(deviceKey, io, extra) {
  const update = { online: true, lastSeen: new Date() };
  if (extra) update.extra = extra;
  // new:false returns the document as it was BEFORE this update, so we can
  // tell a real offline->online transition from a routine heartbeat.
  const prev = await DeviceStatus.findOneAndUpdate(
    { deviceKey },
    { $set: update },
    { upsert: true, new: false }
  );
  io.emit('node_status', { deviceKey, online: true });
  if (deviceKey === 'fridge' && (!prev || !prev.online)) {
    await fridge.logEvent(io, 'CONTROLLER_ONLINE');
  }
}

// Called for a node's Last Will ("offline") - the node is gone right now.
// (Previously any status payload, even "offline", refreshed lastSeen and the
// node only flipped to offline ~20 s later via the watchdog.)
async function markOffline(deviceKey, io) {
  const prev = await DeviceStatus.findOneAndUpdate(
    { deviceKey },
    { $set: { online: false } },
    { upsert: true, new: false }
  );
  io.emit('node_status', { deviceKey, online: false });
  if (deviceKey === 'fridge' && prev && prev.online) {
    await fridge.logEvent(io, 'CONTROLLER_OFFLINE', 'Controller disconnected (Last Will)');
    io.emit('fridge_update', await fridge.getView()); // dashboards switch to UNKNOWN
  }
}

function initMqtt(io) {
  const brokerUrl = process.env.MQTT_BROKER || 'mqtt://127.0.0.1:1883';
  const options = {};
  if (process.env.MQTT_USERNAME) {
    options.username = process.env.MQTT_USERNAME;
    options.password = process.env.MQTT_PASSWORD;
  }

  client = mqtt.connect(brokerUrl, options);

  client.on('connect', () => {
    console.log(`[MQTT] Connected to broker at ${brokerUrl}`);
    client.subscribe(T.ALL, (err) => {
      if (err) console.error('[MQTT] Subscribe error:', err.message);
      else console.log(`[MQTT] Subscribed to ${T.ALL}`);
    });
  });

  client.on('reconnect', () => console.log('[MQTT] Reconnecting...'));
  client.on('error', (err) => console.error('[MQTT] Error:', err.message));

  client.on('message', async (topic, payloadBuf, packet) => {
    const payload = payloadBuf.toString().trim();
    // true only for a retained message replayed by the broker on (re)subscribe -
    // i.e. an old value, NOT proof the publisher is alive right now.
    const isRetainedReplay = !!(packet && packet.retain);
    try {
      const statusKey = deviceKeyFromStatusTopic(topic);
      if (statusKey) {
        if (payload === 'offline') {
          await markOffline(statusKey, io);
        } else {
          await markOnline(statusKey, io, payload === 'online' ? undefined : { raw: payload });
        }
        return;
      }

      // ---------- Fridge node ----------
      if (topic === T.FRIDGE_DOOR || topic === T.FRIDGE_POWER) {
        if (isRetainedReplay) return; // stale snapshot; live telemetry follows within seconds
        await markOnline('fridge', io);
        await fridge.updateState(io, topic === T.FRIDGE_DOOR ? { door: payload } : { power: payload });
        return;
      }

      if (topic === T.FRIDGE_TEMPERATURE || topic === T.FRIDGE_HUMIDITY || topic === T.FRIDGE_SENSOR) {
        if (isRetainedReplay) return;
        await markOnline('fridge', io);
        const value = topic === T.FRIDGE_TEMPERATURE
          ? { temperature: Number(payload) }
          : topic === T.FRIDGE_HUMIDITY
            ? { humidity: Number(payload) }
            : { sensor: payload };
        await fridge.updateState(io, value);
        return;
      }

      if (topic === T.FRIDGE_TELEMETRY) {
        const data = JSON.parse(payload);
        await markOnline('fridge', io);
        await fridge.updateState(io, {
          door: data.door,
          power: data.power,
          doorOpenSeconds: Number(data.door_open_seconds),
          temperature: data.temp_c == null ? null : Number(data.temp_c),
          humidity: data.humidity == null ? null : Number(data.humidity),
          sensor: data.sensor
        });
        return;
      }

      if (topic === T.FRIDGE_ALERT) {
        if (isRetainedReplay) return;
        if (!FRIDGE_NODE_ALERTS.includes(payload)) {
          console.log(`[MQTT] Ignoring unknown fridge alert: ${payload}`);
          return;
        }
        await markOnline('fridge', io);
        await fridge.logEvent(io, payload);
        return;
      }

      if (topic === T.DOOR_EVENT) {
        const data = JSON.parse(payload);
        if (data.event === 'IN' || data.event === 'OUT') {
          const ev = await CustomerEvent.create({ type: data.event });
          await markOnline('door', io);
          io.emit('customer_event', { type: ev.type, timestamp: ev.timestamp });
        }
        return;
      }

      if (topic === T.ENV_DATA) {
        const data = JSON.parse(payload);
        const reading = await SensorReading.create({
          temp: data.temp,
          hum: data.hum,
          smoke: data.smoke
        });
        await markOnline('env', io);
        io.emit('sensor_update', {
          temp: reading.temp,
          hum: reading.hum,
          smoke: reading.smoke,
          timestamp: reading.timestamp
        });
        return;
      }

      if (topic === T.AC_STATE) {
        const data = JSON.parse(payload);
        await markOnline('control', io, { acState: data.state });
        io.emit('ac_status', { state: data.state });
        return;
      }

      if (topic === T.HUB_AGGREGATE) {
        const data = JSON.parse(payload);
        await markOnline('hub', io, data);
        io.emit('hub_aggregate', data);
        return;
      }

      // cafe/camera/upload/done and others: just log
      console.log(`[MQTT] (unhandled) ${topic} -> ${payload}`);
    } catch (err) {
      console.error(`[MQTT] Failed to process message on ${topic}:`, err.message);
    }
  });

  return client;
}

function getClient() {
  if (!client) throw new Error('MQTT client not initialized yet');
  return client;
}

// Publish a manual AC override (dashboard "ON/OFF" toggle)
function publishAcSet(state) {
  getClient().publish(T.AC_SET, JSON.stringify({ state }), { retain: false });
}

// Publish updated thresholds - retained so the hub gets them
// immediately AND on every reboot (even before backend reconnects).
function publishThreshold(onTemp, offTemp) {
  getClient().publish(
    T.THRESHOLD_SET,
    JSON.stringify({ on_temp: onTemp, off_temp: offTemp }),
    { retain: true }
  );
}

module.exports = { initMqtt, getClient, publishAcSet, publishThreshold, TOPICS: T };
