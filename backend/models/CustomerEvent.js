const mongoose = require('mongoose');

const CustomerEventSchema = new mongoose.Schema({
  type: { type: String, enum: ['IN', 'OUT'], required: true },
  timestamp: { type: Date, default: Date.now, index: true }
});

module.exports = mongoose.model('CustomerEvent', CustomerEventSchema);
