// In-page simulator: the probe rules of core/src/probe.c, a pretend board and
// a pretend heartbeat server, with time running 60 times faster.
(() => {
  const SPEED = 60;
  const $ = (id) => document.getElementById(id);
  const fmt = (s) => (s < 60 ? `${s} s` : s < 3600 ? `${Math.floor(s / 60)} min` : `${(s / 3600).toFixed(1)} h`);
  const deg = (v) => `${v < 0 ? '−' : ''}${Math.abs(v).toFixed(1)} °C`;

  const probes = [
    { slug: 'power', type: 'alive', interval: 60, delay: 0 },
    { slug: 'freezer', type: 'temp', interval: 60, delay: 600, max: -15, hyst: 0.5 },
    { slug: 'leak', type: 'contact', interval: 60, delay: 0 },
  ];
  const GRACE = 120;

  let now = 0; // virtual seconds
  let bootAt = 0;
  let powered = true;
  let leak = false;
  let temp = -18;
  let state = probes.map(() => ({ started: false }));
  const server = probes.map(() => ({ last: null, sig: null, alerted: null }));

  // Port of probe_step(): returns null or { sig, body }.
  function step(p, st, r) {
    const wasOk = st.started ? st.condOk : true;
    let ok = true;
    let why = '';
    if (p.type === 'temp') {
      const margin = wasOk ? 0 : p.hyst;
      if (r > p.max - margin) { ok = false; why = 'above max'; }
    } else if (p.type === 'contact' && r) {
      ok = false; why = 'contact active';
    }
    if (!ok && (wasOk || !st.started)) st.badSince = now;
    st.condOk = ok;
    const reportOk = ok || (now - st.badSince < p.delay && (!st.started || st.reportedOk));
    const due = !st.started || reportOk !== st.reportedOk || now - st.lastSent >= p.interval;
    if (!due) return null;
    let body = p.type === 'temp' ? `temp_c=${r.toFixed(2)}` : p.type === 'contact' ? `contact=${r ? 'active' : 'idle'}` : 'alive';
    if (!reportOk) body += ` problem="${why}"`;
    st.started = true; st.reportedOk = reportOk; st.lastSent = now;
    return { sig: reportOk ? 'success' : 'fail', body };
  }

  function log(text, cls) {
    const el = $('log');
    const line = document.createElement('div');
    line.textContent = `${clock()}  ${text}`;
    if (cls) line.className = cls;
    el.append(line);
    while (el.childNodes.length > 200) el.firstChild.remove();
    el.scrollTop = el.scrollHeight;
  }

  function clock() {
    const t = 8 * 3600 + now; // the simulated day starts at 08:00
    const h = Math.floor(t / 3600) % 24;
    const m = Math.floor(t / 60) % 60;
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
  }

  function serverState(s) {
    if (s.last === null) return 'new';
    if (s.sig === 'fail') return 'down';
    const age = now - s.last;
    if (age > 60 + GRACE) return 'down';
    if (age > 60) return 'late';
    return 'up';
  }

  function notify(text, alert) {
    const el = $('phone');
    el.className = `phone${alert ? ' alert' : ''}`;
    el.textContent = `${clock()}  ${text}`;
    log(`notification: ${text}`, alert ? 'alert' : 'ok');
  }

  function tick() {
    now += 1;
    if (powered) {
      const uptime = now - bootAt;
      probes.forEach((p, i) => {
        const r = p.type === 'temp' ? temp : p.type === 'contact' ? leak : true;
        const a = step(p, state[i], r);
        if (!a) return;
        const extra = state[i].sentOnce ? '' : ' boot=power-on';
        state[i].sentOnce = true;
        server[i].last = now;
        server[i].sig = a.sig;
        log(`POST /ping/…/${p.slug}${a.sig === 'fail' ? '/fail' : ''}  ${a.body} uptime_s=${uptime}${extra}`, a.sig === 'fail' ? 'fail' : '');
      });
    }
    probes.forEach((p, i) => {
      const s = server[i];
      const st = serverState(s);
      if (st === 'down' && s.alerted !== 'down') {
        s.alerted = 'down';
        notify(s.sig === 'fail' ? `${p.slug} is DOWN: the device reports a problem` : `${p.slug} is DOWN: no heartbeat for ${fmt(now - s.last)}`, true);
      } else if (st === 'up' && s.alerted === 'down') {
        s.alerted = 'up';
        notify(`${p.slug} is back UP`, false);
      }
    });
  }

  function render() {
    $('dev').textContent = powered ? `uptime ${fmt(now - bootAt)}` : 'no power';
    const problem = state.some((s) => s.started && !s.reportedOk);
    const led = $('led');
    led.className = `led${powered && (problem ? Math.floor(now / 2) % 2 === 0 : now % 3 === 0) ? ' on' : ''}`;
    $('checks').innerHTML = probes.map((p, i) => {
      const s = server[i];
      const st = serverState(s);
      const age = s.last === null ? 'waiting' : `last ping ${fmt(now - s.last)} ago`;
      return `<li><span><b>${p.slug}</b> <span class="small">${age}</span></span><span class="state ${st}">${st}</span></li>`;
    }).join('');
  }

  $('power').addEventListener('click', (e) => {
    powered = !powered;
    e.currentTarget.setAttribute('aria-pressed', String(powered));
    e.currentTarget.textContent = powered ? 'Power on' : 'Power cut';
    if (powered) {
      bootAt = now;
      state = probes.map(() => ({ started: false }));
      log('power restored: booting', 'ok');
    } else {
      log('power cut: the board is off, nothing is sent', 'fail');
    }
  });
  $('leak').addEventListener('click', (e) => {
    leak = !leak;
    e.currentTarget.setAttribute('aria-pressed', String(leak));
  });
  $('temp').addEventListener('input', (e) => {
    temp = Number(e.target.value);
    $('tempv').textContent = deg(temp);
  });

  setInterval(() => {
    for (let i = 0; i < SPEED / 10; i++) tick();
    render();
  }, 100);
  render();
})();
