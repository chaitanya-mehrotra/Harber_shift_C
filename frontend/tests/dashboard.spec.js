import { test, expect } from '@playwright/test';

const nav = (page, name) => page.getByRole('navigation', { name: 'Main navigation' }).getByRole('button', { name, exact: true });
const editor = (page) => page.locator('form.controls-grid');
const outcome = (page) => page.getByRole('complementary', { name: 'Destination arrival outcome' });
const tracker = (page) => page.getByRole('region', { name: 'Voyage tracker' });
const calendar = (page) => page.getByRole('region', { name: 'Closure window and schedule overlap' });
const estimates = (page) => page.getByRole('region', { name: 'Delay estimation breakdown' });

async function openDashboard(page) {
  await page.goto('/');
  await page.getByRole('button', { name: 'Start simulation' }).click();
}

async function openShipmentStrike(page) {
  await openDashboard(page);
  await nav(page, 'Shipments').click();
  await page.getByRole('combobox', { name: 'Dataset' }).selectOption('sample');
  await page.getByRole('button', { name: /SH011 · C011/ }).click();
  const itinerary = page.getByRole('region', { name: 'Shipment itinerary detail' });
  await expect(itinerary.locator('.stop-card')).toHaveCount(4);
  await itinerary.getByRole('button', { name: 'Simulate disruption here: Durban, 2027-01-29' }).click();
  await expect(page.getByRole('tab', { name: 'Manual event' })).toHaveAttribute('aria-selected', 'true');
  await expect(editor(page).getByRole('combobox', { name: 'Port', exact: true })).toHaveValue('P05');
  await expect(editor(page).locator('input[type=date]').last()).toHaveValue('2027-01-29');
  await editor(page).getByLabel('Simulated date', { exact: true }).fill('2027-01-28');
  await editor(page).getByRole('button', { name: 'Apply event' }).click();
  await expect(editor(page).getByRole('status')).toContainText('1 shipment affected.');
}

test('manufacturing overview shows customer dates and penalty, and reset clears the event', async ({ page }, testInfo) => {
  const errors = [];
  const publicRequests = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route(/^https?:\/\/(?!127\.0\.0\.1:5173\/)/, async (route) => {
    publicRequests.push(route.request().url());
    await route.abort();
  });

  await openDashboard(page);
  await expect(page.locator('html')).toHaveCSS('color-scheme', 'dark');
  await expect(page.getByRole('heading', { name: 'Manufacturing overview' })).toBeVisible();
  await expect(outcome(page)).toContainText('Arrival: 17 Jan 2027');
  await expect(outcome(page)).toContainText('Penalty: ₹0');
  await expect(page.locator('.overview-disruption-card')).toHaveCount(0);
  const milestones = page.getByRole('list', { name: 'Manufacturing milestones' });
  await expect(milestones).toContainText('Manufacture parts in Durban');
  await expect(milestones.getByRole('listitem').last()).toContainText('17 Jan');
  const trail = page.locator('.voyage-trail').first();
  await expect(trail).toHaveCSS('animation-name', 'voyage-draw');
  await page.getByRole('button', { name: 'Replay voyage' }).click();
  await expect(trail).toHaveCSS('animation-name', 'voyage-draw');
  await page.getByRole('button', { name: 'Simulate a disruption' }).click();
  await editor(page).getByRole('button', { name: 'Apply event' }).click();
  await expect(editor(page).getByRole('status')).toContainText('strike at Durban');
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('Arrival: 19 Jan 2027');
  await expect(outcome(page)).toContainText('Penalty: ₹5,000');
  await expect(milestones.getByRole('listitem').last()).toContainText('19 Jan');
  await expect(tracker(page).locator('.tracker-row').first().locator('.tracker-stop')).toHaveText([
    'Savannah1 Jan', 'Durban · arrival8 Jan', 'Durban · departure12 Jan', 'Mumbai17 Jan',
  ]);
  await expect(tracker(page).locator('.tracker-row').last().locator('.tracker-stop')).toHaveText([
    'Savannah1 Jan', 'Durban · arrival10 Jan', 'Durban · departure14 Jan', 'Mumbai19 Jan',
  ]);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  if (process.env.HARBORSHIFT_CAPTURE) {
    await page.screenshot({ path: `../docs/screenshots/${testInfo.project.name}.png`, fullPage: true });
  }
  const recoveryButton = outcome(page).getByRole('button', { name: /Compare recovery options/ });
  if (testInfo.project.use.hasTouch) await recoveryButton.tap();
  else await recoveryButton.click();
  const wait = page.locator('.recovery-card').filter({ hasText: 'Wait for Durban to reopen' });
  await expect(wait).toContainText('19 Jan 2027 (2 days delayed)');
  await wait.getByRole('button', { name: 'Select Wait' }).click();
  await expect(wait.getByRole('button', { name: 'Selected: Wait' })).toHaveAttribute('aria-pressed', 'true');

  await nav(page, 'Disruptions').click();
  await editor(page).getByRole('button', { name: 'Reset', exact: true }).click();
  await expect(editor(page).getByRole('status')).toHaveCount(0);
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('Arrival: 17 Jan 2027');
  await expect(outcome(page)).toContainText('Penalty: ₹0');
  expect(errors).toEqual([]);
  expect(publicRequests).toEqual([]);
});

test('network finds the 32-day alternative and handles an unreachable destination', async ({ page }) => {
  await openDashboard(page);
  await nav(page, 'Network').click();
  await expect(page.getByRole('heading', { name: 'Route lookup' })).toBeVisible();
  await page.getByText('Required stops and closures', { exact: true }).click();
  await page.getByLabel('Closed port IDs').fill('P05');
  await page.getByRole('button', { name: 'Find route' }).click();
  await expect(page.getByRole('status')).toContainText('R36, R33, R08 · 32 days');
  await page.getByRole('combobox', { name: /^Origin/ }).selectOption('P11');
  await page.getByRole('combobox', { name: /^Destination/ }).selectOption('P03');
  await page.getByRole('button', { name: 'Find route' }).click();
  await expect(page.getByRole('status')).toContainText('No route.');
});

test('public advisories are opt-in', async ({ page }) => {
  let requests = 0;
  await page.route('**/api/news', async (route) => {
    requests += 1;
    await route.fulfill({ json: { articles: [], errors: [] } });
  });
  await openDashboard(page);
  await nav(page, 'Disruptions').click();
  const fetchAdvisories = page.getByRole('button', { name: 'Fetch public advisories' });
  await expect(fetchAdvisories).toBeVisible();
  expect(requests).toBe(0);
  await fetchAdvisories.click();
  await expect(page.getByText('No matching disruption candidates.')).toBeVisible();
  expect(requests).toBe(1);
});

test('manual event reports invalid fields and updates zero-impact and delayed results', async ({ page }) => {
  await openDashboard(page);
  await nav(page, 'Disruptions').click();
  await page.getByRole('tab', { name: 'Manual event' }).click();
  const form = editor(page);
  const duration = form.getByLabel('Duration · days');
  const start = form.locator('input[type=date]').last();
  await form.getByRole('combobox', { name: 'Port', exact: true }).selectOption('P11');
  await form.getByRole('combobox', { name: 'Event type' }).selectOption('severe_weather');
  await form.getByLabel('Simulated date', { exact: true }).fill('2027-01-13');
  await start.fill('2027-01-21');
  await duration.fill('');
  await form.getByRole('button', { name: 'Apply event' }).click();
  await expect(form.getByRole('alert')).toContainText('Duration · days');
  await expect(duration).toBeFocused();

  await duration.fill('3');
  await expect(form.getByRole('status')).toContainText('No shipments were affected.');
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('On schedule');

  await nav(page, 'Disruptions').click();
  await start.fill('2027-01-17');
  await expect(form.getByRole('status')).toContainText('1 shipment affected.');
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('Arrival: 20 Jan 2027');
  await expect(outcome(page)).toContainText('Penalty: ₹10,000');
});

test('newer preset remains active when an older simulation response is delayed', async ({ page }) => {
  await openDashboard(page);
  await nav(page, 'Disruptions').click();
  const select = editor(page).getByRole('combobox', { name: /^Preset event/ });
  await select.selectOption('E002');
  await expect(editor(page).getByRole('status')).toContainText('at Singapore');

  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  await page.route('**/api/simulate', async (route) => {
    if (route.request().postDataJSON().sample_ids?.includes('E004')) await gate;
    await route.continue().catch(() => {});
  });
  try {
    const olderRequest = page.waitForRequest((request) =>
      request.url().endsWith('/api/simulate') && request.postDataJSON().sample_ids?.includes('E004'));
    await select.selectOption('E004');
    await olderRequest;
    await select.selectOption('E001');
    await expect(editor(page).getByRole('status')).toContainText('strike at Durban');
  } finally {
    release();
  }
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('19 Jan 2027');
  await expect(page.locator('.overview-disruption-card')).toContainText('Durban');
});

test('SH011 closure has accurate dates and separates the supplier and original arrival', async ({ page }) => {
  await openShipmentStrike(page);
  await expect(calendar(page)).toContainText('Reopens 3 Feb');
  await expect(calendar(page)).toContainText('of wait until 3 Feb');
  await expect(estimates(page).locator('.ml-highlight')).toContainText('Closed ports use the reopening date.');
  await expect(estimates(page).locator('.ml-pill')).toHaveCount(0);
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('25 Feb 2027');
  await expect(outcome(page)).toContainText('5 days delayed');
  await expect(outcome(page)).toContainText('Penalty: ₹5,000');
  await expect(tracker(page).locator('.tracker-row').last()).toContainText('Port Louis22 Feb');
  await outcome(page).getByRole('button', { name: /Compare recovery options/ }).click();
  const supplier = page.locator('.recovery-card').filter({ hasText: 'Use replacement supplier S17' });
  const wait = page.locator('.recovery-card').filter({ hasText: 'Wait for Durban to reopen' });
  await expect(supplier).toContainText('31 Jan 2027');
  await expect(supplier.locator('.recovery-row').filter({ hasText: 'Estimated penalty' })).toContainText('₹0');
  await expect(wait.locator('.recovery-row').filter({ hasText: 'Estimated penalty' })).toContainText('₹5,000');
});

test('SH011 restricted forecasts use the model inputs and the actual wait date', async ({ page }) => {
  await openShipmentStrike(page);
  await editor(page).getByLabel('Port status').selectOption('restricted');
  await expect(calendar(page)).toContainText('Restriction ends 3 Feb');
  await expect(calendar(page)).toContainText('of wait until 31 Jan');
  const analysis = estimates(page);
  await expect(analysis.locator('.ml-pill')).toHaveCount(7);
  await expect(analysis.locator('.ml-pill').filter({ hasText: 'Planned transit' })).toHaveText('Planned transit: 23 days');
  await expect(analysis.locator('.estimate-col').nth(1)).toContainText('Arrival: 22 Feb 2027');
  await expect(analysis.locator('.ml-highlight')).toContainText('Raw prediction: 7.4 days');
  await expect(analysis.locator('.ml-highlight')).toContainText('Rounded wait: 8 days');
  await expect(analysis.locator('.ml-highlight')).toContainText('Arrival: 28 Feb 2027');

  await editor(page).getByText('Packing and delay assumptions', { exact: true }).click();
  await editor(page).getByLabel('Use synthetic ML for restrictions').check();
  await expect(page.locator('.model-tag')).toHaveText('Synthetic ML');
  await expect(calendar(page)).toContainText('of wait until 6 Feb');
  await nav(page, 'Overview').click();
  await expect(outcome(page)).toContainText('Arrival: 28 Feb 2027');
  await expect(outcome(page)).toContainText('Penalty: ₹20,000');
});

test('the Network globe rotates, zooms, resets, and focuses a port', async ({ page }, info) => {
  await openDashboard(page);
  await nav(page, 'Network').click();
  const globe = page.getByRole('region', { name: 'Rotatable shipping globe' });
  const land = globe.locator('.globe-land');
  const ocean = globe.locator('.globe-ocean');
  await expect(land).toBeVisible();
  await globe.getByRole('button', { name: 'Reset view' }).click();
  const initialLand = await land.getAttribute('d');
  const initialOcean = await ocean.getAttribute('d');
  await globe.press('ArrowRight');
  await expect.poll(() => land.getAttribute('d')).not.toBe(initialLand);
  await globe.getByRole('button', { name: 'Zoom in' }).click();
  await expect.poll(() => ocean.getAttribute('d')).not.toBe(initialOcean);
  await globe.getByRole('button', { name: 'Reset view' }).click();
  await expect.poll(() => land.getAttribute('d')).toBe(initialLand);

  await page.getByRole('button', { name: 'Show all ports' }).click();
  await globe.getByRole('button', { name: 'Go to port' }).click();
  await globe.getByRole('combobox', { name: /^Go to port/ }).selectOption('P16');
  await expect(globe.getByRole('button', { name: 'Singapore' })).toHaveAttribute('aria-pressed', 'true');
  if (process.env.HARBORSHIFT_GLOBE_CAPTURE) {
    await page.locator('.globe-workspace').screenshot({
      path: `${process.env.HARBORSHIFT_GLOBE_CAPTURE}/HarborShift-globe-${info.project.name}.png`,
    });
  }
});

test('Dijkstra playback follows the C search and restores the final route', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await openDashboard(page);
  await nav(page, 'Network').click();
  await page.getByText('Required stops and closures', { exact: true }).click();
  await page.getByLabel('Closed port IDs').fill('P05');
  const response = page.waitForResponse((res) => res.url().endsWith('/api/route'));
  await page.getByRole('button', { name: 'Find route' }).click();
  const result = await (await response).json();
  expect(result.search_trace[0].port_id).toBe('P03');
  expect(result.search_trace.at(-1)).toMatchObject({ port_id: 'P11', weight: 32 });
  expect(result.search_trace.some((step) => step.port_id === 'P05')).toBe(false);

  await page.getByRole('link', { name: 'Watch the search on the globe' }).click();
  const playback = page.getByRole('region', { name: 'Dijkstra playback' });
  await playback.getByRole('button', { name: 'Next step' }).click();
  await expect(playback).toContainText('Selected Savannah');
  await expect(page.locator('.globe-port.search-current')).toHaveAttribute('aria-label', 'Savannah');
  await expect(page.locator('.globe-frame .edge.search-update').first()).toBeVisible();
  const pulse = page.locator('.search-line-beacon').first();
  await expect(pulse).toBeAttached();
  await expect(pulse).toBeHidden();
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await expect(pulse).toHaveCSS('animation-name', 'voyage-sail');
  await expect(pulse).toBeVisible();
  await page.emulateMedia({ reducedMotion: 'reduce' });
  for (const step of result.search_trace.slice(1)) {
    await playback.getByRole('button', { name: 'Next step' }).click();
  }
  await expect(playback).toContainText('Selected Mumbai');
  await playback.getByRole('button', { name: 'Next step' }).click();
  await expect(playback).toContainText('Search complete. R36');
  await expect(page.locator('.globe-frame .edge.chosen').first()).toBeVisible();
  await playback.getByRole('button', { name: 'Previous step' }).click();
  await expect(playback).toContainText('Selected Mumbai');
});

test('welcome page waits for Start before showing the dashboard or loading a route', async ({ page }) => {
  const calls = [];
  page.on('request', (request) => {
    if (new URL(request.url()).pathname.startsWith('/api/')) calls.push(request.url());
  });
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'HarborShift' })).toBeVisible();
  await expect(page.getByRole('navigation')).toHaveCount(0);
  await expect(page.getByLabel('Shipment journey route map')).toHaveCount(0);
  await expect(outcome(page)).toHaveCount(0);
  expect(calls).toEqual([]);
  await page.getByRole('button', { name: 'Start simulation' }).click();
  await expect(outcome(page)).toContainText('Arrival: 17 Jan 2027');
  await expect(page.locator('.overview-disruption-card')).toHaveCount(0);
});
