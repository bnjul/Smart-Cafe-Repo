const socket = io();

// ---------------- Theme ----------------
const themeToggle = document.getElementById('theme-toggle');
const systemTheme = window.matchMedia('(prefers-color-scheme: dark)');

function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  const dark = theme === 'dark';
  themeToggle.textContent = dark ? 'Bright mode' : 'Dark mode';
  themeToggle.setAttribute('aria-label', dark ? 'Switch to bright theme' : 'Switch to dark theme');
}

applyTheme(document.documentElement.dataset.theme || (systemTheme.matches ? 'dark' : 'light'));
themeToggle.addEventListener('click', () => {
  const nextTheme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  localStorage.setItem('cafe-theme', nextTheme);
  applyTheme(nextTheme);
});
systemTheme.addEventListener('change', (event) => {
  if (!localStorage.getItem('cafe-theme')) applyTheme(event.matches ? 'dark' : 'light');
});

// ---------------- Clock ----------------
function tickClock() {
  document.getElementById('clock').textContent = new Date().toLocaleString();
}
setInterval(tickClock, 1000);
tickClock();

// ---------------- Connection indicator ----------------
socket.on('connect', () => {
  const el = document.getElementById('conn-indicator');
  el.textContent = 'Live';
  el.className = 'badge badge-online';
});
socket.on('disconnect', () => {
  const el = document.getElementById('conn-indicator');
  el.textContent = 'Disconnected';
  el.className = 'badge badge-offline';
});

// ---------------- Node status ----------------
const NODE_KEYS = ['hub', 'door', 'env', 'control', 'camera', 'fridge'];

function renderNodeStatus(nodes) {
  let activeCount = 0;
  NODE_KEYS.forEach((key) => {
    const node = nodes.find((n) => n.deviceKey === key);
    const online = !!(node && node.online);
    if (online) activeCount++;
    const pill = document.querySelector(`.node-pill[data-node="${key}"]`);
    if (pill) pill.classList.toggle('online', online);
    if (key === 'fridge') {
      fridgeState.online = online;
      renderFridge();
    }
  });
  document.getElementById('nodes-active').textContent = `${activeCount} / ${NODE_KEYS.length}`;
}

function setNodeOnline(deviceKey, online) {
  const pill = document.querySelector(`.node-pill[data-node="${deviceKey}"]`);
  if (pill) pill.classList.toggle('online', online);
  // Recompute the count from the DOM
  const activeCount = document.querySelectorAll('.node-pill.online').length;
  document.getElementById('nodes-active').textContent = `${activeCount} / ${NODE_KEYS.length}`;
  if (deviceKey === 'fridge') {
    fridgeState.online = online;
    renderFridge();
  }
}

socket.on('node_status', (data) => setNodeOnline(data.deviceKey, data.online));

// ---------------- Sensor readings ----------------
function renderSensors(data) {
  if (data.temp !== undefined) document.getElementById('env-temp').textContent = Number(data.temp).toFixed(1);
  if (data.hum !== undefined) document.getElementById('env-hum').textContent = Number(data.hum).toFixed(1);
  if (data.smoke !== undefined) document.getElementById('env-smoke').textContent = data.smoke;
}
socket.on('sensor_update', (data) => {
  renderSensors(data);
  pushChartPoint(data);
});

// ---------------- AC status ----------------
function renderAc(state) {
  const badge = document.getElementById('ac-badge');
  badge.textContent = state;
  badge.className = state === 'ON' ? 'badge-on' : 'badge-off';
}
socket.on('ac_status', (data) => renderAc(data.state));

document.getElementById('ac-on-btn').addEventListener('click', () => setAc('ON'));
document.getElementById('ac-off-btn').addEventListener('click', () => setAc('OFF'));
async function setAc(state) {
  await fetch('/api/control/ac', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ state })
  });
}

// ---------------- Customer counters ----------------
function renderCustomers({ in: inCount, out, inside }) {
  document.getElementById('cust-in').textContent = inCount;
  document.getElementById('cust-out').textContent = out;
  document.getElementById('cust-inside').textContent = inside;
}
socket.on('customer_event', refreshCustomerSummary);
async function refreshCustomerSummary() {
  const res = await fetch('/api/customers/summary');
  renderCustomers(await res.json());
}

// ---------------- Threshold settings ----------------
async function loadThreshold() {
  const res = await fetch('/api/settings/threshold');
  const data = await res.json();
  document.getElementById('on-temp-input').value = data.onTemp;
  document.getElementById('off-temp-input').value = data.offTemp;
}

document.getElementById('save-threshold-btn').addEventListener('click', async () => {
  const onTemp = parseFloat(document.getElementById('on-temp-input').value);
  const offTemp = parseFloat(document.getElementById('off-temp-input').value);
  const statusEl = document.getElementById('threshold-status');

  if (isNaN(onTemp) || isNaN(offTemp)) {
    statusEl.textContent = 'Enter valid numbers.';
    return;
  }
  const res = await fetch('/api/settings/threshold', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ onTemp, offTemp })
  });
  if (res.ok) {
    statusEl.textContent = `Saved. AC turns ON at ${onTemp}°C, OFF at ${offTemp}°C.`;
  } else {
    const err = await res.json();
    statusEl.textContent = `Error: ${err.error}`;
  }
});

// ---------------- Chart ----------------
let historyChart;
function initChart(readings) {
  // Chart.js comes from a CDN. If the browser has no internet (e.g. a cafe LAN
  // that lost its uplink), skip the chart instead of aborting the rest of the
  // dashboard (thresholds, photos, customer counters) with an exception.
  if (typeof Chart === 'undefined') {
    console.warn('Chart.js did not load - history chart disabled');
    const canvas = document.getElementById('history-chart');
    if (canvas && canvas.parentElement) {
      const note = document.createElement('p');
      note.className = 'hint';
      note.textContent = 'History chart unavailable: the chart library could not be loaded (no internet access in this browser).';
      canvas.replaceWith(note);
    }
    return;
  }
  const ctx = document.getElementById('history-chart').getContext('2d');
  historyChart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: readings.map((r) => new Date(r.timestamp).toLocaleTimeString()),
      datasets: [
        {
          label: 'Temp (°C)',
          data: readings.map((r) => r.temp),
          borderColor: '#c9793d',
          backgroundColor: 'rgba(201,121,61,0.12)',
          tension: 0.3,
          yAxisID: 'y'
        },
        {
          label: 'Humidity (%)',
          data: readings.map((r) => r.hum),
          borderColor: '#7fa88a',
          backgroundColor: 'rgba(127,168,138,0.1)',
          tension: 0.3,
          yAxisID: 'y1'
        }
      ]
    },
    options: {
      responsive: true,
      interaction: { mode: 'index', intersect: false },
      scales: {
        y: { type: 'linear', position: 'left', ticks: { color: '#b6a58c' }, grid: { color: 'rgba(241,232,220,0.06)' } },
        y1: { type: 'linear', position: 'right', ticks: { color: '#b6a58c' }, grid: { drawOnChartArea: false } },
        x: { ticks: { color: '#b6a58c', maxTicksLimit: 8 }, grid: { display: false } }
      },
      plugins: { legend: { labels: { color: '#f1e8dc' } } }
    }
  });
}

function pushChartPoint(reading) {
  if (!historyChart) return;
  historyChart.data.labels.push(new Date(reading.timestamp || Date.now()).toLocaleTimeString());
  historyChart.data.datasets[0].data.push(reading.temp);
  historyChart.data.datasets[1].data.push(reading.hum);
  if (historyChart.data.labels.length > 100) {
    historyChart.data.labels.shift();
    historyChart.data.datasets[0].data.shift();
    historyChart.data.datasets[1].data.shift();
  }
  historyChart.update('none');
}

// ---------------- Photo history ----------------
let photoHistory = [];
let photosExpanded = false;
let hasMorePhotos = false;
let loadingMorePhotos = false;
const PHOTO_PAGE_SIZE = 30;

function photoMarkup(photo) {
  return `
    <div class="photo-item" data-photo-id="${photo._id}">
      <input class="photo-select" type="checkbox" aria-label="Select photo captured ${new Date(photo.timestamp).toLocaleString()}" />
      <img src="${photo.url}" alt="Customer ${photo.reason} capture" loading="lazy" />
      <div class="photo-hover-actions">
        <button class="photo-action-btn" type="button" data-view-photo aria-label="View photo">View</button>
        <button class="photo-action-btn photo-action-delete" type="button" data-delete-photo aria-label="Delete photo">Delete</button>
      </div>
      <div class="photo-meta">
        <span class="reason-${photo.reason.toLowerCase()}">${photo.reason}</span>
        &middot; ${new Date(photo.timestamp).toLocaleString()}
      </div>
    </div>
  `;
}

function findPhoto(photoId) {
  return photoHistory.find((photo) => photo._id === photoId);
}

function openPhotoModal(photo) {
  const modal = document.getElementById('photo-modal');
  document.getElementById('photo-modal-image').src = photo.url;
  document.getElementById('photo-modal-image').alt = `Customer ${photo.reason} capture`;
  document.getElementById('photo-modal-title').textContent = `${photo.reason} capture`;
  modal.dataset.photoId = photo._id;
  modal.hidden = false;
  document.body.classList.add('photo-modal-open');
}

function closePhotoModal() {
  const modal = document.getElementById('photo-modal');
  modal.hidden = true;
  document.getElementById('photo-modal-image').removeAttribute('src');
  document.body.classList.remove('photo-modal-open');
}

async function deletePhotos(ids) {
  const response = await fetch('/api/photos', {
    method: 'DELETE',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ids })
  });
  if (!response.ok) {
    const error = await response.json().catch(() => ({}));
    throw new Error(error.error || 'Failed to delete selected photos');
  }
  photoHistory = photoHistory.filter((photo) => !ids.includes(photo._id));
  renderPhotos(photoHistory);
}

function updatePhotoActions() {
  const selected = document.querySelectorAll('.photo-select:checked').length;
  const deleteButton = document.getElementById('delete-photos-btn');
  const selectAll = document.getElementById('select-all-photos');
  deleteButton.disabled = selected === 0;
  selectAll.checked = photoHistory.length > 0 && selected === photoHistory.length;
  selectAll.indeterminate = selected > 0 && selected < photoHistory.length;
}

function updateViewMoreButton() {
  const button = document.getElementById('view-more-photos-btn');
  const grid = document.getElementById('photo-grid');
  const hasOlderPhotos = photoHistory.length > 0 && grid.scrollHeight > grid.clientHeight + 1;
  button.hidden = !hasOlderPhotos && !(photosExpanded && hasMorePhotos);
  button.textContent = photosExpanded
    ? (hasMorePhotos ? 'Load older photos' : 'Show less')
    : 'View more';
}

function renderPhotos(photos) {
  photoHistory = photos;
  const grid = document.getElementById('photo-grid');
  if (!photos.length) {
    grid.innerHTML = '<p class="hint">No photos yet.</p>';
    grid.classList.remove('expanded');
    photosExpanded = false;
    hasMorePhotos = false;
    updateViewMoreButton();
    updatePhotoActions();
    return;
  }
  grid.innerHTML = photos.map(photoMarkup).join('');
  grid.classList.toggle('expanded', photosExpanded);
  updatePhotoActions();
  requestAnimationFrame(updateViewMoreButton);
}

document.getElementById('photo-grid').addEventListener('change', updatePhotoActions);
document.getElementById('photo-grid').addEventListener('click', async (event) => {
  const item = event.target.closest('.photo-item');
  if (!item) return;
  const photo = findPhoto(item.dataset.photoId);
  if (!photo) return;
  if (event.target.closest('[data-view-photo]') || event.target.matches('img')) {
    openPhotoModal(photo);
    return;
  }
  const deleteButton = event.target.closest('[data-delete-photo]');
  if (!deleteButton) return;
  if (!window.confirm('Delete this photo?')) return;
  deleteButton.disabled = true;
  try {
    await deletePhotos([photo._id]);
  } catch (error) {
    console.error('Photo deletion failed:', error);
    window.alert(error.message);
    deleteButton.disabled = false;
  }
});
document.getElementById('select-all-photos').addEventListener('change', (event) => {
  document.querySelectorAll('.photo-select').forEach((checkbox) => {
    checkbox.checked = event.target.checked;
  });
  updatePhotoActions();
});
document.getElementById('view-more-photos-btn').addEventListener('click', async () => {
  if (!photosExpanded) {
    photosExpanded = true;
    document.getElementById('photo-grid').classList.add('expanded');
    updateViewMoreButton();
    return;
  }
  if (hasMorePhotos) {
    if (loadingMorePhotos) return;
    loadingMorePhotos = true;
    const button = document.getElementById('view-more-photos-btn');
    button.disabled = true;
    button.textContent = 'Loading…';
    try {
      const response = await fetch(`/api/photos?limit=${PHOTO_PAGE_SIZE}&skip=${photoHistory.length}`);
      if (!response.ok) throw new Error('Failed to load older photos');
      const olderPhotos = await response.json();
      photoHistory = [...photoHistory, ...olderPhotos];
      hasMorePhotos = olderPhotos.length === PHOTO_PAGE_SIZE;
      renderPhotos(photoHistory);
    } catch (error) {
      console.error('Loading older photos failed:', error);
      window.alert(error.message);
      updateViewMoreButton();
    } finally {
      loadingMorePhotos = false;
      button.disabled = false;
      updateViewMoreButton();
    }
    return;
  }
  photosExpanded = false;
  document.getElementById('photo-grid').classList.remove('expanded');
  updateViewMoreButton();
});
document.getElementById('delete-photos-btn').addEventListener('click', async () => {
  const ids = [...document.querySelectorAll('.photo-select:checked')]
    .map((checkbox) => checkbox.closest('.photo-item').dataset.photoId);
  if (!ids.length) return;
  if (!window.confirm(`Delete ${ids.length} selected photo${ids.length === 1 ? '' : 's'}?`)) return;

  const button = document.getElementById('delete-photos-btn');
  button.disabled = true;
  try {
    await deletePhotos(ids);
  } catch (error) {
    console.error('Photo deletion failed:', error);
    window.alert(error.message);
    updatePhotoActions();
  }
});

document.querySelectorAll('[data-close-photo-modal]').forEach((element) => {
  element.addEventListener('click', closePhotoModal);
});
document.getElementById('photo-modal-delete').addEventListener('click', async () => {
  const modal = document.getElementById('photo-modal');
  const photoId = modal.dataset.photoId;
  if (!photoId || !window.confirm('Delete this photo?')) return;
  const button = document.getElementById('photo-modal-delete');
  button.disabled = true;
  try {
    await deletePhotos([photoId]);
    closePhotoModal();
  } catch (error) {
    console.error('Photo deletion failed:', error);
    window.alert(error.message);
  } finally {
    button.disabled = false;
  }
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !document.getElementById('photo-modal').hidden) closePhotoModal();
});

socket.on('new_photo', (photo) => {
  photoHistory = [photo, ...photoHistory.filter((item) => item._id !== photo._id)];
  renderPhotos(photoHistory);
});
socket.on('photos_deleted', ({ ids }) => {
  photoHistory = photoHistory.filter((photo) => !ids.includes(photo._id));
  renderPhotos(photoHistory);
});

// ---------------- Refrigerator ----------------
// door/power are only shown as real values while the controller is online AND
// its data is fresh. Otherwise they read UNKNOWN - never assumed OFF/CLOSED.
const fridgeState = {
  online: false,
  door: 'UNKNOWN',
  power: 'UNKNOWN',
  doorOpenSeconds: 0,
  temperature: null,
  humidity: null,
  sensor: 'UNKNOWN',
  lastUpdate: null,     // ISO string from the server
  receivedAt: 0,        // browser-clock time the data was last known to be fresh
  staleAfterMs: 35000
};
const FRIDGE_WARN_AFTER_S = 5; // matches the firmware's first-warning delay

const FRIDGE_EVENT_INFO = {
  DOOR_OPEN_WARNING:  { label: 'Door left open',      sev: 'warn' },
  SUPPLY_LOST:        { label: 'Supply lost',         sev: 'bad'  },
  SUPPLY_RESTORED:    { label: 'Supply restored',     sev: 'good' },
  TEMP_HIGH:          { label: 'Temperature high',    sev: 'bad'  },
  TEMP_NORMAL:        { label: 'Temperature normal',  sev: 'good' },
  SENSOR_FAULT:       { label: 'Sensor fault',        sev: 'bad'  },
  SENSOR_OK:          { label: 'Sensor recovered',    sev: 'good' },
  CONTROLLER_OFFLINE: { label: 'Controller offline',  sev: 'bad'  },
  CONTROLLER_ONLINE:  { label: 'Controller online',   sev: 'good' }
};

function applyFridgeView(view) {
  fridgeState.online = !!view.controllerOnline;
  fridgeState.door = view.door;
  fridgeState.power = view.power;
  fridgeState.doorOpenSeconds = view.doorOpenSeconds || 0;
  fridgeState.temperature = view.temperature;
  fridgeState.humidity = view.humidity;
  fridgeState.sensor = view.sensor;
  fridgeState.lastUpdate = view.lastUpdate;
  fridgeState.staleAfterMs = view.staleAfterMs || fridgeState.staleAfterMs;
  fridgeState.receivedAt = view.ageMs != null ? Date.now() - view.ageMs : 0;
  renderFridge();
}

function fridgeEffective() {
  const fresh = fridgeState.receivedAt > 0 && Date.now() - fridgeState.receivedAt < fridgeState.staleAfterMs;
  const trusted = fridgeState.online && fresh;
  let openSecs = null;
  if (trusted && fridgeState.door === 'OPEN') {
    openSecs = fridgeState.doorOpenSeconds + Math.floor((Date.now() - fridgeState.receivedAt) / 1000);
  }
  return {
    controllerUp: trusted,
    door: trusted ? fridgeState.door : 'UNKNOWN',
    power: trusted ? fridgeState.power : 'UNKNOWN',
    openSecs
  };
}

function formatDuration(totalSeconds) {
  const s = Math.max(0, Math.floor(totalSeconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = String(s % 60).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

function renderFridge() {
  const eff = fridgeEffective();

  const powerEl = document.getElementById('fridge-power');
  powerEl.textContent = eff.power;
  powerEl.className = 'badge ' + (eff.power === 'ON' ? 'badge-on' : eff.power === 'OFF' ? 'badge-off' : 'badge-unknown');

  const doorEl = document.getElementById('fridge-door');
  doorEl.textContent = eff.door;
  doorEl.className = 'badge ' + (eff.door === 'CLOSED' ? 'badge-on' : eff.door === 'OPEN' ? 'badge-warn' : 'badge-unknown');

  const durEl = document.getElementById('fridge-duration');
  if (eff.openSecs !== null) {
    durEl.textContent = formatDuration(eff.openSecs);
    durEl.classList.toggle('warn', eff.openSecs >= FRIDGE_WARN_AFTER_S);
  } else {
    durEl.textContent = eff.door === 'CLOSED' ? '0:00' : '—';
    durEl.classList.remove('warn');
  }

  const ctrlEl = document.getElementById('fridge-controller');
  ctrlEl.textContent = eff.controllerUp ? 'Controller ONLINE' : 'Controller OFFLINE';
  ctrlEl.className = 'badge ' + (eff.controllerUp ? 'badge-online' : 'badge-offline');

  document.getElementById('fridge-last-update').textContent = fridgeState.lastUpdate
    ? `Last valid update: ${new Date(fridgeState.lastUpdate).toLocaleString()}`
    : 'No update received yet';

  const temperatureEl = document.getElementById('fridge-temperature');
  temperatureEl.textContent = eff.controllerUp && fridgeState.temperature != null
    ? `${Number(fridgeState.temperature).toFixed(1)} °C`
    : '—';
  const humidityEl = document.getElementById('fridge-humidity');
  humidityEl.textContent = eff.controllerUp && fridgeState.humidity != null
    ? `${Number(fridgeState.humidity).toFixed(1)} %`
    : '—';
  const sensorEl = document.getElementById('fridge-sensor');
  sensorEl.textContent = eff.controllerUp ? fridgeState.sensor : 'UNKNOWN';
  sensorEl.className = 'badge ' + (eff.controllerUp && fridgeState.sensor === 'OK' ? 'badge-on' : 'badge-unknown');
}
setInterval(renderFridge, 1000); // ticks the open-duration counter and expires stale data

function buildFridgeAlertItem(ev) {
  const info = FRIDGE_EVENT_INFO[ev.type] || { label: ev.type, sev: 'warn' };
  const li = document.createElement('li');
  li.className = `alert-item sev-${info.sev}`;
  const name = document.createElement('span');
  name.className = 'alert-name';
  name.textContent = info.label;
  const time = document.createElement('span');
  time.className = 'alert-time';
  time.textContent = new Date(ev.timestamp).toLocaleString();
  li.append(name, time);
  return li;
}

function renderFridgeEvents(events) {
  const list = document.getElementById('fridge-alerts');
  list.innerHTML = '';
  if (!events.length) {
    const li = document.createElement('li');
    li.className = 'hint';
    li.textContent = 'No alerts yet.';
    list.appendChild(li);
    return;
  }
  events.forEach((ev) => list.appendChild(buildFridgeAlertItem(ev)));
}

function addFridgeEvent(ev) {
  const list = document.getElementById('fridge-alerts');
  const placeholder = list.querySelector('.hint');
  if (placeholder) list.innerHTML = '';
  list.prepend(buildFridgeAlertItem(ev));
  while (list.children.length > 50) list.removeChild(list.lastChild);
}

socket.on('fridge_update', applyFridgeView);
socket.on('fridge_event', addFridgeEvent);

// ---------------- Initial load ----------------
async function init() {
  const [statusRes, sensorRes, historyRes, customerRes, photoRes, fridgeRes, fridgeEventsRes] = await Promise.all([
    fetch('/api/status'),
    fetch('/api/sensors/latest'),
    fetch('/api/sensors/history?hours=24'),
    fetch('/api/customers/summary'),
    fetch(`/api/photos?limit=${PHOTO_PAGE_SIZE}`),
    fetch('/api/fridge/state'),
    fetch('/api/fridge/events?limit=30')
  ]);

  const fridgeView = await fridgeRes.json();
  applyFridgeView(fridgeView);
  renderFridgeEvents(await fridgeEventsRes.json());

  const status = await statusRes.json();
  renderNodeStatus(status.nodes);

  const latest = await sensorRes.json();
  renderSensors(latest);

  const history = await historyRes.json();
  initChart(history);

  const customers = await customerRes.json();
  renderCustomers(customers);

  const photos = await photoRes.json();
  hasMorePhotos = photos.length === PHOTO_PAGE_SIZE;
  renderPhotos(photos);

  await loadThreshold();

  // Poll node status every 5s as a fallback in case a socket event is missed
  setInterval(async () => {
    const res = await fetch('/api/status');
    const data = await res.json();
    renderNodeStatus(data.nodes);
  }, 5000);
}

init();
