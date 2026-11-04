const mongoose = require('mongoose');

const FRIDGE_EVENT_TYPES = [
  'DOOR_OPEN_WARNING',   // from the node: door left open past the warning delay
  'SUPPLY_LOST',         // from the node: validated AC loss at the monitored point
  'SUPPLY_RESTORED',     // from the node: validated AC return
  'TEMP_HIGH',            // from the node: temperature above the configured limit
  'TEMP_NORMAL',          // from the node: temperature returned to normal
  'SENSOR_FAULT',         // from the node: DHT readings failed repeatedly
  'SENSOR_OK',            // from the node: DHT readings recovered
  'CONTROLLER_OFFLINE',  // derived by the backend: Last Will or heartbeat timeout
  'CONTROLLER_ONLINE'    // derived by the backend: controller came back
];

const FridgeEventSchema = new mongoose.Schema({
  type: { type: String, enum: FRIDGE_EVENT_TYPES, required: true },
  detail: { type: String, default: '' },
  timestamp: { type: Date, default: Date.now, index: true }
});

module.exports = mongoose.model('FridgeEvent', FridgeEventSchema);
module.exports.FRIDGE_EVENT_TYPES = FRIDGE_EVENT_TYPES;
