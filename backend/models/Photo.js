const mongoose = require('mongoose');

const PhotoSchema = new mongoose.Schema({
  filename: { type: String, required: true },
  url: { type: String, required: true },
  reason: { type: String, enum: ['IN', 'OUT'], required: true },
  eventId: { type: String },
  index: { type: Number },
  device: { type: String },
  timestamp: { type: Date, default: Date.now, index: true }
});

module.exports = mongoose.model('Photo', PhotoSchema);
