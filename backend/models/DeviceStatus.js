const mongoose = require('mongoose');

const DeviceStatusSchema = new mongoose.Schema({
  deviceKey: { type: String, required: true, unique: true }, // hub | door | env | control | camera
  online: { type: Boolean, default: false },
  lastSeen: { type: Date, default: Date.now },
  extra: { type: mongoose.Schema.Types.Mixed, default: {} } // e.g. { acState: "ON" }
});

module.exports = mongoose.model('DeviceStatus', DeviceStatusSchema);
