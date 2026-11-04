const FridgeState = require('../models/FridgeState');
const FridgeEvent = require('../models/FridgeEvent');
const DeviceStatus = require('../models/DeviceStatus');

// If no valid update arrives for this long, the dashboard treats the
// fridge sensor states as UNKNOWN (the node heartbeats every 5-10 s).
const FRIDGE_STALE_MS = 35000;

const DOOR_VALUES = ['OPEN', 'CLOSED', 'UNKNOWN'];
const POWER_VALUES = ['ON', 'OFF', 'UNKNOWN'];
const SENSOR_VALUES = ['OK', 'FAULT'];

async function logEvent(io, type, detail = '') {
  const ev = await FridgeEvent.create({ type, detail });
  io.emit('fridge_event', { _id: ev._id, type: ev.type, detail: ev.detail, timestamp: ev.timestamp });
  return ev;
}

// Merge validated fields into the single state document and notify dashboards.
// Unknown/invalid values are ignored rather than stored.
async function updateState(io, patch) {
  const set = { lastUpdate: new Date() };
  if (DOOR_VALUES.includes(patch.door)) set.door = patch.door;
  if (POWER_VALUES.includes(patch.power)) set.power = patch.power;
  if (Number.isFinite(patch.doorOpenSeconds) && patch.doorOpenSeconds >= 0) {
    set.doorOpenSeconds = Math.floor(patch.doorOpenSeconds);
  }
  if (Number.isFinite(patch.temperature) && patch.temperature >= -40 && patch.temperature <= 80) {
    set.temperature = patch.temperature;
  } else if (patch.temperature === null) {
    set.temperature = null;
  }
  if (Number.isFinite(patch.humidity) && patch.humidity >= 0 && patch.humidity <= 100) {
    set.humidity = patch.humidity;
  } else if (patch.humidity === null) {
    set.humidity = null;
  }
  if (SENSOR_VALUES.includes(patch.sensor)) set.sensor = patch.sensor;
  // A closed door always means a zero open-duration
  if (set.door === 'CLOSED') set.doorOpenSeconds = 0;
  if (set.sensor === 'FAULT') {
    set.temperature = null;
    set.humidity = null;
  }

  const st = await FridgeState.findOneAndUpdate(
    { key: 'main' },
    { $set: set },
    { upsert: true, new: true }
  );
  const view = await toView(st);
  io.emit('fridge_update', view);
  return view;
}

// What the API/dashboard sees. Door/power are reported as UNKNOWN when the
// controller is offline or its data is stale - never assumed OFF/CLOSED.
async function toView(st) {
  const node = await DeviceStatus.findOne({ deviceKey: 'fridge' });
  const lastUpdateMs = st && st.lastUpdate ? new Date(st.lastUpdate).getTime() : 0;
  const fresh = lastUpdateMs > 0 && Date.now() - lastUpdateMs < FRIDGE_STALE_MS;
  const controllerOnline = !!(node && node.online);
  const trusted = controllerOnline && fresh;

  return {
    controllerOnline,
    door: trusted && st ? st.door : 'UNKNOWN',
    power: trusted && st ? st.power : 'UNKNOWN',
    doorOpenSeconds: trusted && st && st.door === 'OPEN' ? st.doorOpenSeconds : 0,
    temperature: trusted && st && st.sensor === 'OK' ? st.temperature : null,
    humidity: trusted && st && st.sensor === 'OK' ? st.humidity : null,
    sensor: trusted && st && SENSOR_VALUES.includes(st.sensor) ? st.sensor : 'UNKNOWN',
    lastUpdate: st && st.lastUpdate ? st.lastUpdate : null,
    // Age of the data in ms by the SERVER clock, so a browser with a skewed clock still judges freshness correctly
    ageMs: lastUpdateMs > 0 ? Date.now() - lastUpdateMs : null,
    staleAfterMs: FRIDGE_STALE_MS
  };
}

async function getView() {
  const st = await FridgeState.findOne({ key: 'main' });
  return toView(st);
}

module.exports = { logEvent, updateState, getView, FRIDGE_STALE_MS };
