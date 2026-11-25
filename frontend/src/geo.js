// Shared synthetic port coordinates [longitude, latitude] and date formatting helpers.
export const coordinates = {
  P01: [-74.03, 40.67], P02: [-95.27, 29.73], P03: [-81.15, 32.13], P04: [-118.26, 33.74],
  P05: [31.04, -29.88], P06: [18.44, -33.91], P07: [25.63, -33.96], P08: [39.65, -4.06],
  P09: [39.29, -6.83], P10: [0.0, 5.63], P11: [72.84, 18.94], P12: [69.72, 22.74],
  P13: [80.30, 13.09], P14: [76.26, 9.96], P15: [83.29, 17.69], P16: [103.82, 1.27],
  P17: [79.84, 6.95], P18: [55.05, 25.01], P19: [4.10, 51.96], P20: [57.49, -20.15],
};

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const DAY_MS = 86400000;

export function parseIsoDate(iso) {
  if (!iso) return null;
  const [y, m, d] = iso.split('-').map(Number);
  if (!y || !m || !d) return null;
  return new Date(Date.UTC(y, m - 1, d));
}

export function addDays(iso, delta) {
  const dt = parseIsoDate(iso);
  if (!dt) return iso;
  dt.setUTCDate(dt.getUTCDate() + delta);
  return dt.toISOString().slice(0, 10);
}

export function daysBetween(later, earlier) {
  const a = parseIsoDate(later);
  const b = parseIsoDate(earlier);
  if (!a || !b) return 0;
  return Math.round((a.getTime() - b.getTime()) / DAY_MS);
}

export function formatShortDate(iso) {
  const dt = parseIsoDate(iso);
  if (!dt) return iso || '—';
  return `${dt.getUTCDate()} ${MONTHS[dt.getUTCMonth()]}`;
}

export function formatFullDate(iso) {
  const dt = parseIsoDate(iso);
  if (!dt) return iso || '—';
  return `${dt.getUTCDate()} ${MONTHS[dt.getUTCMonth()]} ${dt.getUTCFullYear()}`;
}

export function formatRange(startIso, endIso) {
  const start = parseIsoDate(startIso);
  const end = parseIsoDate(endIso);
  if (!start || !end) return `${startIso || '—'} – ${endIso || '—'}`;
  if (start.getUTCFullYear() === end.getUTCFullYear()) {
    return `${formatShortDate(startIso)} – ${formatFullDate(endIso)}`;
  }
  return `${formatFullDate(startIso)} – ${formatFullDate(endIso)}`;
}
