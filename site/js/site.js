/* The Compact site: rim clock, day storyline, face explorer, breathing ring. No dependencies. */
(function () {
  'use strict';
  var doc = document.documentElement;
  var reduce = window.matchMedia('(prefers-reduced-motion: reduce)');
  var SVGNS = 'http://www.w3.org/2000/svg';

  /* ---------- motion switch (respects reduced motion) ---------- */
  var motionOn = !reduce.matches;
  var toggles = document.querySelectorAll('[data-motion-toggle]');
  function applyMotion() {
    doc.setAttribute('data-motion', motionOn ? 'on' : 'off');
    toggles.forEach(function (b) {
      b.setAttribute('aria-pressed', motionOn ? 'false' : 'true');
      b.textContent = motionOn ? 'Pause animation' : 'Play animation';
    });
    document.querySelectorAll('svg.flow').forEach(function (s) {
      if (!s.pauseAnimations) return;
      if (motionOn) s.unpauseAnimations(); else s.pauseAnimations();
    });
  }
  toggles.forEach(function (b) { b.addEventListener('click', function () { motionOn = !motionOn; applyMotion(); tickMoons(); }); });
  if (reduce.addEventListener) reduce.addEventListener('change', function (e) { motionOn = !e.matches; applyMotion(); });

  /* ---------- the rim: 60 dots, a planet at a clock time, a moon for seconds ---------- */
  var rims = [];
  function el(name, attrs, parent) {
    var n = document.createElementNS(SVGNS, name);
    for (var k in attrs) n.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(n);
    return n;
  }
  function hoursOf(t) { var p = t.split(':'); return +p[0] + (+p[1]) / 60; }
  function buildRim(svg) {
    var R = 470, C = 500;
    for (var i = 0; i < 60; i++) {
      var a = i * 6 * Math.PI / 180, q = i % 15 === 0, h = i % 5 === 0;
      el('circle', { cx: (C + R * Math.sin(a)).toFixed(2), cy: (C - R * Math.cos(a)).toFixed(2), r: q ? 5.2 : h ? 4 : 2.8, 'class': 'rim__dot' + (q ? ' rim__dot--q' : '') }, svg);
    }
    var hand = el('g', { 'class': 'rim__hand' }, svg);
    el('circle', { cx: C, cy: C - R, r: 26, 'class': 'rim__orbit' }, hand);
    el('circle', { cx: C, cy: C - R, r: 10, 'class': 'rim__planet' }, hand);
    var arm = el('g', {}, hand);
    el('circle', { cx: C, cy: C - R - 26, r: 6, 'class': 'rim__moon' }, arm);
    var rim = { svg: svg, hand: hand, arm: arm, C: C, R: R };
    setRim(rim, svg.getAttribute('data-time') || '10:12', true);
    rims.push(rim);
    return rim;
  }
  function setRim(rim, time, instant) {
    var deg = hoursOf(time) * 30;          /* continuous through the day, so the planet always moves forward */
    if (instant) rim.hand.style.transition = 'none';
    rim.hand.style.transform = 'rotate(' + deg + 'deg)';
    if (instant) { rim.hand.getBoundingClientRect(); rim.hand.style.transition = ''; }
  }
  function tickMoons() {
    var s = motionOn ? new Date().getSeconds() : 0;   /* parked at 12 o'clock when motion is off, as on the device */
    rims.forEach(function (r) { r.arm.setAttribute('transform', 'rotate(' + s * 6 + ' ' + r.C + ' ' + (r.C - r.R) + ')'); });
  }
  document.querySelectorAll('svg[data-rim]').forEach(buildRim);
  tickMoons();
  (function schedule() { setTimeout(function () { tickMoons(); schedule(); }, 1000 - (Date.now() % 1000) + 5); })();

  /* ---------- nav ---------- */
  var nav = document.querySelector('.nav');
  var toggle = document.querySelector('.nav__toggle');
  var links = document.getElementById('nav-links');
  if (toggle && links) {
    toggle.addEventListener('click', function () {
      var open = toggle.getAttribute('aria-expanded') !== 'true';
      toggle.setAttribute('aria-expanded', String(open));
      links.classList.toggle('is-open', open);
      document.body.style.overflow = open ? 'hidden' : '';
    });
    links.addEventListener('click', function (e) {
      if (e.target.closest('a')) { toggle.setAttribute('aria-expanded', 'false'); links.classList.remove('is-open'); document.body.style.overflow = ''; }
    });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && links.classList.contains('is-open')) { toggle.click(); toggle.focus(); } });
  }
  function onScroll() { if (nav) nav.classList.toggle('is-scrolled', window.scrollY > 8); }
  window.addEventListener('scroll', onScroll, { passive: true }); onScroll();

  /* ---------- a day: the sticky stage follows the step in view ---------- */
  var stage = document.querySelector('.day__stage');
  var steps = document.querySelectorAll('.day__steps .step');
  var stageRim = rims.filter(function (r) { return r.svg.hasAttribute('data-stage-rim'); })[0];
  function activate(step) {
    steps.forEach(function (s) { s.classList.toggle('is-active', s === step); });
    if (!stage) return;
    var shot = step.getAttribute('data-shot');
    stage.querySelectorAll('.day__shot').forEach(function (img) { img.classList.toggle('is-active', img.getAttribute('data-shot') === shot); });
    if (stageRim) setRim(stageRim, step.getAttribute('data-time'), false);
  }
  if ('IntersectionObserver' in window && steps.length) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) { if (e.isIntersecting) activate(e.target); });
    }, { rootMargin: '-45% 0px -45% 0px' });
    steps.forEach(function (s) { io.observe(s); });
    activate(steps[0]);
  } else { steps.forEach(function (s) { s.classList.add('is-active'); }); }

  /* ---------- faces: swipe like the device ---------- */
  var box = document.querySelector('[data-faces]');
  if (box) {
    var faces = {}; box.querySelectorAll('.face').forEach(function (f) { faces[f.getAttribute('data-face')] = f; });
    var btns = document.querySelectorAll('.facemap__btn');
    var descs = document.querySelectorAll('.facedesc__item');
    var cur = 'now';
    /* swipe direction (where the finger moves) from each face, as on the Compact */
    var graph = {
      now: { right: 'day', left: 'agents', up: 'earlier', down: 'settings' },
      day: { left: 'now', tap: 'card' },
      card: { down: 'day' },
      agents: { right: 'now' },
      earlier: { down: 'now' },
      settings: { up: 'now' }
    };
    /* the gesture that reaches each face from Now (or from Day for the card), used for map clicks */
    var arrive = { day: 'right', agents: 'left', earlier: 'up', settings: 'down', card: 'tap', now: null };
    var leave = { day: 'left', agents: 'right', earlier: 'down', settings: 'up', card: 'down' };
    var vec = { left: [-1, 0], right: [1, 0], up: [0, -1], down: [0, 1], tap: [0, 0] };
    function show(next, gesture) {
      if (!faces[next] || next === cur) return;
      var a = faces[cur], b = faces[next], v = vec[gesture] || [0, 0];
      var animate = motionOn && !reduce.matches && (v[0] || v[1]);
      [a, b].forEach(function (f) { f.classList.remove('is-in', 'is-out'); });
      if (animate) {
        /* content follows the finger: the new face enters from the side the finger moved away from */
        b.style.setProperty('--fx', (-v[0] * 100) + '%'); b.style.setProperty('--fy', (-v[1] * 100) + '%');
        a.style.setProperty('--fx', (v[0] * 100) + '%'); a.style.setProperty('--fy', (v[1] * 100) + '%');
        b.classList.add('is-in'); a.classList.add('is-out');
        setTimeout(function () { a.classList.remove('is-out'); b.classList.remove('is-in'); }, 340);
      }
      a.classList.remove('is-active'); b.classList.add('is-active');
      cur = next;
      btns.forEach(function (x) { var on = x.getAttribute('data-go') === cur; x.classList.toggle('is-active', on); x.setAttribute('aria-pressed', String(on)); });
      descs.forEach(function (d) { d.classList.toggle('is-active', d.getAttribute('data-desc') === cur); });
    }
    function gesture(g) { var to = (graph[cur] || {})[g]; if (to) show(to, g === 'tap' ? 'up' : g); }
    function go(target) {
      if (target === cur) return;
      if (target === 'now') { show('now', leave[cur] || 'left'); return; }
      if (target === 'card' && cur !== 'day') { show('card', 'up'); return; }
      if (cur !== 'now' && target !== 'card') { show(target, arrive[target]); return; }
      show(target, arrive[target] === 'tap' ? 'up' : arrive[target]);
    }
    btns.forEach(function (b) { b.addEventListener('click', function () { go(b.getAttribute('data-go')); }); });
    var sx = 0, sy = 0, down = false;
    box.addEventListener('pointerdown', function (e) { down = true; sx = e.clientX; sy = e.clientY; });
    box.addEventListener('pointerup', function (e) {
      if (!down) return; down = false;
      var dx = e.clientX - sx, dy = e.clientY - sy, ax = Math.abs(dx), ay = Math.abs(dy);
      if (ax < 10 && ay < 10) { gesture('tap'); return; }
      if (Math.max(ax, ay) < 40) return;
      gesture(ax > ay ? (dx > 0 ? 'right' : 'left') : (dy > 0 ? 'down' : 'up'));
    });
    box.addEventListener('pointercancel', function () { down = false; });
    box.addEventListener('keydown', function (e) {
      var map = { ArrowLeft: 'right', ArrowRight: 'left', ArrowUp: 'up', ArrowDown: 'down' };
      if (map[e.key]) {
        e.preventDefault();
        var g = map[e.key];
        if (cur === 'earlier' && e.key === 'ArrowDown') g = 'down';
        if (cur === 'settings' && e.key === 'ArrowUp') g = 'up';
        if (cur === 'now' && e.key === 'ArrowUp') g = 'up';
        if (cur === 'now' && e.key === 'ArrowDown') g = 'down';
        gesture(g);
      } else if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); gesture('tap'); }
      else if (e.key === 'Escape') { e.preventDefault(); go('now'); }
    });
  }

  /* ---------- breathing ring ---------- */
  var br = document.querySelector('[data-breathe]');
  if (br) {
    br.querySelectorAll('.breathe__controls button').forEach(function (b) {
      b.addEventListener('click', function () {
        br.setAttribute('data-weight', b.getAttribute('data-w'));
        br.querySelectorAll('.breathe__controls button').forEach(function (x) { x.setAttribute('aria-pressed', String(x === b)); });
      });
    });
  }

  applyMotion();
  requestAnimationFrame(function () { requestAnimationFrame(function () { doc.classList.add('is-ready'); }); });
})();
