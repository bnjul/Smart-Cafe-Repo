const mongoose = require('mongoose');

// Single document ("main") holding the latest known fridge state.
const FridgeStateSchema = new mongoose.Schema({
  key: { type: String, required: true, unique: true, default: 'main' },
  door: { type: String, enum: ['OPEN', 'CLOSED', 'UNKNOWN'], default: 'UNKNOWN' },
  power: { type: String, enum: ['ON', 'OFF', 'UNKNOWN'], default: 'UNKNOWN' },
  doorOpenSeconds: { type: Number, default: 0 },
  temperature: { type: Number, default: null },
  humidity: { type: Number, default: null },
  sensor: { type: String, enum: ['OK', 'FAULT'], default: 'OK' },
  // When the last VALID update from the fridge controller was received
  lastUpdate: { type: Date, default: null }
});

module.exports = mongoose.model('FridgeState', FridgeStateSchema);
