const palette = { sain: '#2E86AB', infecte: '#E63946', isole: '#F4A261', retabli: '#2A9D8F' };

const el = {
  runList: document.getElementById('runList'),
  runCount: document.getElementById('runCount'),
  health: document.getElementById('health'),
  formPanel: document.getElementById('formPanel'),
  resultPanel: document.getElementById('resultPanel'),
  emptyState: document.getElementById('emptyState'),
  resultTitle: document.getElementById('resultTitle'),
  resultMeta: document.getElementById('resultMeta'),
  resultStats: document.getElementById('resultStats'),
  runForm: document.getElementById('runForm'),
  btnNewRun: document.getElementById('btnNewRun'),
  btnDelete: document.getElementById('btnDelete'),
};

let chart = null;
let activeRunId = null;

async function api(path, options) {
  const res = await fetch(path, options);
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(body.error || ('erreur HTTP ' + res.status));
  }
  return res.json();
}

async function checkHealth() {
  try {
    const h = await api('/api/health');
    el.health.textContent = h.status === 'ok'
      ? 'serveur en ligne · base connectée'
      : 'serveur dégradé (moteur ou base indisponible)';
    el.health.className = 'health ' + (h.status === 'ok' ? 'ok' : 'bad');
  } catch (e) {
    el.health.textContent = 'serveur injoignable';
    el.health.className = 'health bad';
  }
}

function fmtDate(iso) {
  const d = new Date(iso);
  return d.toLocaleString('fr-FR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
}

async function loadRunList() {
  const runs = await api('/api/runs?limit=50');
  el.runCount.textContent = runs.length;
  el.runList.innerHTML = '';

  if (runs.length === 0) {
    el.runList.innerHTML = '<p class="run-empty">Aucune simulation enregistrée pour l\'instant.</p>';
    return;
  }

  for (const r of runs) {
    const btn = document.createElement('button');
    btn.className = 'run-item' + (r.id === activeRunId ? ' active' : '');
    btn.innerHTML = `
      <div class="rn-top"><span>N=${r.population.toLocaleString('fr-FR')}</span><span>#${r.id}</span></div>
      <div class="rn-bottom">${fmtDate(r.created_at)} · ${r.threads}thr · ${r.exec_time_s.toFixed(3)}s</div>
    `;
    btn.addEventListener('click', () => showRun(r.id));
    el.runList.appendChild(btn);
  }
}

function renderChart(series) {
  const ctx = document.getElementById('chartCourbe');
  if (chart) chart.destroy();
  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: series.map(s => s.step),
      datasets: [
        { label: 'Sain', data: series.map(s => s.sain), borderColor: palette.sain, borderWidth: 2, pointRadius: 0, tension: 0.15 },
        { label: 'Infecté', data: series.map(s => s.infecte), borderColor: palette.infecte, borderWidth: 2, pointRadius: 0, tension: 0.15 },
        { label: 'Isolé', data: series.map(s => s.isole), borderColor: palette.isole, borderWidth: 2, pointRadius: 0, tension: 0.15 },
        { label: 'Rétabli', data: series.map(s => s.retabli), borderColor: palette.retabli, borderWidth: 2, pointRadius: 0, tension: 0.15 },
      ]
    },
    options: {
      responsive: true, maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: { legend: { display: false } },
      scales: {
        x: { title: { display: true, text: 'pas de temps' }, grid: { display: false }, ticks: { maxTicksLimit: 10 } },
        y: { title: { display: true, text: "nombre d'individus" }, grid: { color: '#D7DEDA' } }
      }
    }
  });
}

function renderStats(run) {
  el.resultStats.innerHTML = `
    <div class="stat"><span class="num">${run.exec_time_s.toFixed(3)}s</span><span class="label">temps de simulation (${run.threads} threads)</span></div>
    <div class="stat"><span class="num">${run.final_retabli.toLocaleString('fr-FR')}</span><span class="label">rétablis en fin de simulation</span></div>
    <div class="stat"><span class="num">${run.final_infecte + run.final_isole}</span><span class="label">encore actifs (infectés + isolés)</span></div>
    <div class="stat"><span class="num">${run.population.toLocaleString('fr-FR')}</span><span class="label">population totale (N)</span></div>
  `;
}

async function showRun(id) {
  activeRunId = id;
  el.emptyState.classList.add('hidden');
  el.resultPanel.classList.remove('hidden');
  el.resultTitle.textContent = `Simulation #${id}`;
  el.resultMeta.textContent = 'chargement…';

  const run = await api('/api/runs/' + id);
  el.resultMeta.textContent =
    `β=${run.beta}  γ=${run.gamma_infecte}  isolement=${run.isolation_prob}/pas  ` +
    `mobilité=${run.mobility}  rayon=${run.radius}  seed=${run.seed} — ${fmtDate(run.created_at)}`;

  renderChart(run.series);
  renderStats(run);
  loadRunList();
}

el.runForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(el.runForm);
  const payload = {};
  for (const [k, v] of fd.entries()) payload[k] = Number(v);

  const btn = el.runForm.querySelector('.run-submit');
  const original = btn.textContent;
  btn.textContent = 'Simulation en cours…';
  btn.disabled = true;

  try {
    const run = await api('/api/runs', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    el.formPanel.classList.add('hidden');
    activeRunId = run.id;
    el.emptyState.classList.add('hidden');
    el.resultPanel.classList.remove('hidden');
    el.resultTitle.textContent = `Simulation #${run.id}`;
    el.resultMeta.textContent =
      `β=${run.beta}  γ=${run.gamma_infecte}  isolement=${run.isolation_prob}/pas  ` +
      `mobilité=${run.mobility}  rayon=${run.radius}  seed=${run.seed} — ${fmtDate(run.created_at)}`;
    renderChart(run.series);
    renderStats(run);
    loadRunList();
  } catch (err) {
    alert('Échec de la simulation : ' + err.message);
  } finally {
    btn.textContent = original;
    btn.disabled = false;
  }
});

el.btnNewRun.addEventListener('click', () => {
  el.formPanel.classList.remove('hidden');
  el.resultPanel.classList.add('hidden');
  el.emptyState.classList.add('hidden');
});

el.btnDelete.addEventListener('click', async () => {
  if (!activeRunId) return;
  if (!confirm('Supprimer cette simulation ?')) return;
  await api('/api/runs/' + activeRunId, { method: 'DELETE' });
  el.resultPanel.classList.add('hidden');
  el.emptyState.classList.remove('hidden');
  activeRunId = null;
  loadRunList();
});

(async function init() {
  await checkHealth();
  await loadRunList();
  const runs = await api('/api/runs?limit=1');
  if (runs.length > 0) {
    el.formPanel.classList.add('hidden');
    await showRun(runs[0].id);
  } else {
    el.emptyState.classList.remove('hidden');
  }
})();
