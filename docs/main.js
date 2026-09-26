// Fill every mockup slot from its <template>
document.querySelectorAll('.js-mock').forEach((slot) => {
  const tpl = document.getElementById(slot.dataset.tpl);
  if (tpl) slot.appendChild(tpl.content.cloneNode(true));
});

// Hero before/after slider
const compare = document.getElementById('compare');
const range = document.getElementById('compare-range');

function setPos(value) {
  compare.style.setProperty('--pos', value + '%');
}

range.addEventListener('input', () => setPos(range.value));

// One sweep on load so it's obvious the divider moves; skipped for reduced motion
// and cancelled as soon as the visitor touches the slider.
const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
if (!reduceMotion) {
  let cancelled = false;
  const stop = () => { cancelled = true; };
  range.addEventListener('pointerdown', stop, { once: true });
  range.addEventListener('keydown', stop, { once: true });

  const keyframes = [[0, 50], [700, 50], [1400, 72], [2300, 50]];
  let start;
  const ease = (t) => t < .5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2;

  function frame(now) {
    if (cancelled) return;
    if (start === undefined) start = now;
    const t = now - start;
    const i = keyframes.findIndex(([at]) => at > t);
    if (i === -1) { setPos(50); range.value = 50; return; }
    const [t0, v0] = keyframes[i - 1];
    const [t1, v1] = keyframes[i];
    const v = v0 + (v1 - v0) * ease((t - t0) / (t1 - t0));
    setPos(v);
    range.value = Math.round(v);
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}
