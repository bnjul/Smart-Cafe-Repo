const mongoose = require('mongoose');

const SensorReadingSchema = new mongoose.Schema({
  temp: { type: Number, required: true },
  hum: { type: Number, required: true },
  smoke: { type: Number, required: true },
  timestamp: { type: Date, default: Date.now, index: true }
});

module.exports = mongoose.model('SensorReading', SensorReadingSchema);
