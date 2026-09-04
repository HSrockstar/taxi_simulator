const canvas = document.getElementById('heatmap');
const context = canvas.getContext('2d');
const tooltip = document.getElementById('tooltip');
const statusDot = document.getElementById('status-dot');
const statusText = document.getElementById('status-text');
const pauseButton = document.getElementById('pause');
let latest = null;
let lastLogSequence = 0;

function heatColor(pending, idle) {
  const difference = pending - idle;
  if (difference > 0) {
    const strength = Math.min(1, difference / 5);
    return `rgb(${Math.round(55 + 200 * strength)},${Math.round(38 + 32 * (1 - strength))},${Math.round(48 + 30 * (1 - strength))})`;
  }
  if (difference < 0) {
    const strength = Math.min(1, -difference / 3);
    return `rgb(${Math.round(31 + 22 * (1 - strength))},${Math.round(70 + 155 * strength)},${Math.round(65 + 76 * strength)})`;
  }
  return pending > 0 ? '#725837' : '#102129';
}

function draw(snapshot) {
  const cell = canvas.width / 100;
  context.clearRect(0, 0, canvas.width, canvas.height);
  for (let index = 0; index < 10000; index += 1) {
    const x = (index % 100) * cell;
    const y = Math.floor(index / 100) * cell;
    context.fillStyle = heatColor(snapshot.pending[index], snapshot.idle[index]);
    context.fillRect(x, y, cell - 0.35, cell - 0.35);
  }

  for (const driver of snapshot.drivers) {
    if (driver.state === 'SERVING') continue;
    context.beginPath();
    context.arc(driver.x / 1000 * canvas.width, driver.y / 1000 * canvas.height,
      driver.state === 'REBALANCING' ? 2.8 : 1.8, 0, Math.PI * 2);
    context.fillStyle = driver.state === 'REBALANCING' ? '#45d7e7' : '#d8fff0';
    context.fill();
  }
}

function updateMetrics(snapshot) {
  const metrics = snapshot.metrics;
  document.getElementById('tick').textContent = `模拟时间 ${snapshot.tick} 秒 · 热点区域 ${snapshot.hotspotIndex + 1}`;
  document.getElementById('queue').textContent = metrics.queueLength;
  document.getElementById('rate').textContent = `${metrics.successRate.toFixed(1)}%`;
  document.getElementById('matched').textContent = metrics.matched;
  document.getElementById('cancelled').textContent = metrics.cancelled;
  document.getElementById('average').textContent = metrics.averageMatchMicros.toFixed(1);
  document.getElementById('total').textContent = metrics.totalMatchMicros;
  pauseButton.textContent = snapshot.paused ? '继续模拟' : '暂停模拟';
}

function updateLogs(logs) {
  const list = document.getElementById('log-list');
  if (!logs.length) return;
  if (lastLogSequence === 0) list.innerHTML = '';
  for (const entry of logs) {
    if (entry.sequence <= lastLogSequence) continue;
    const line = document.createElement('p');
    line.textContent = entry.message;
    if (entry.message.includes('派单成功')) line.className = 'success';
    if (entry.message.includes('订单取消')) line.className = 'cancel';
    if (entry.message.includes('运力调度')) line.className = 'dispatch';
    list.appendChild(line);
    lastLogSequence = entry.sequence;
  }
  while (list.children.length > 50) list.removeChild(list.firstChild);
  list.scrollTop = list.scrollHeight;
}

async function refresh() {
  try {
    const response = await fetch('/api/snapshot', { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    latest = await response.json();
    draw(latest);
    updateMetrics(latest);
    updateLogs(latest.logs);
    statusDot.className = 'online';
    statusText.textContent = latest.paused ? '引擎已暂停' : '引擎运行中';
  } catch (error) {
    statusDot.className = 'offline';
    statusText.textContent = '连接已断开';
  }
}

async function control(action) {
  await fetch(`/api/control/${action}`, { method: 'POST' });
  if (action === 'reset') lastLogSequence = 0;
  setTimeout(refresh, 120);
}

pauseButton.addEventListener('click', () => control(latest && latest.paused ? 'resume' : 'pause'));
document.getElementById('reset').addEventListener('click', () => control('reset'));

canvas.addEventListener('mousemove', event => {
  if (!latest) return;
  const bounds = canvas.getBoundingClientRect();
  const cellX = Math.min(99, Math.floor((event.clientX - bounds.left) / bounds.width * 100));
  const cellY = Math.min(99, Math.floor((event.clientY - bounds.top) / bounds.height * 100));
  const index = cellY * 100 + cellX;
  tooltip.style.display = 'block';
  tooltip.style.left = `${event.clientX - bounds.left + 14}px`;
  tooltip.style.top = `${event.clientY - bounds.top + 14}px`;
  tooltip.textContent = `网格 (${cellX}, ${cellY}) · 订单 ${latest.pending[index]} · 空车 ${latest.idle[index]}`;
});
canvas.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });

refresh();
setInterval(refresh, 500);
