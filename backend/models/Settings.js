const mongoose = require('mongoose');

const SettingsSchema = new mongoose.Schema({
  key: { type: String, required: true, unique: true, default: 'main' },
  onTemp: { type: Number, default: 24 },
  offTemp: { type: Number, default: 18 }
});

SettingsSchema.statics.getSettings = async function () {
  let doc = await this.findOne({ key: 'main' });
  if (!doc) {
    doc = await this.create({ key: 'main', onTemp: 24, offTemp: 18 });
  }
  return doc;
};

module.exports = mongoose.model('Settings', SettingsSchema);
