const mongoose = require('mongoose');

async function connectDB() {
  const uri = process.env.MONGO_URI || 'mongodb://127.0.0.1:27017/cafe_iot';
  try {
    await mongoose.connect(uri);
    console.log(`[DB] Connected to MongoDB (${uri})`);
  } catch (err) {
    console.error('[DB] MongoDB connection error:', err.message);
    console.error('[DB] Is MongoDB running? See TROUBLESHOOTING.md');
    process.exit(1);
  }
}

module.exports = connectDB;
