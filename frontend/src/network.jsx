import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { api, cost } from './client.js';
import { Field } from './controls.jsx';
import { addDays, formatShortDate } from './geo.js';
import { ShippingGlobe } from './globe.jsx';
import './globe.css';

const demoPorts = new Set(['P03', 'P05', 'P11']);
const order = ['available', 'focused', 'affected', 'chosen', 'search-update'];

export function Network({
  ports,
  routes,
  affected,
  affectedPorts,
  selected,
  focusDemo,
  report,
  onEditDisruption,
  search,
}) {
  const [portId, setPortId] = useState('P05');
  const [showAll, setShowAll] = useState(false);
  const [playback, setPlayback] = useState(null);
  const controls = useRef(null);
  const current = playback?.search === search ? playback : null;
  const steps = search?.steps ?? [];
  const step = current ? steps[current.index] : null;
  const finished = current?.index === steps.length;
  const settled = new Set();
  if (step) {
    for (const previous of steps.slice(0, current.index + 1)) {
      if (previous.segment === step.segment) settled.add(previous.port_id);
    }
  }
  const updates = new Set(step?.updates.map((update) => update.route_id));
  const closedPorts = current ? search.closedPorts : [];

  function changeStep(index, playing = false) {
    setPlayback({ search, index, playing });
  }

  useEffect(() => {
    if (!current?.playing || current.index >= steps.length) return;
    const timer = setTimeout(() => {
      const next = current.index + 1;
      setPlayback({ search, index: next, playing: next < steps.length });
    }, 1050);
    return () => clearTimeout(timer);
  }, [current, search, steps.length]);
  const eventPort = affectedPorts[0];
  useLayoutEffect(() => {
    setPortId(eventPort ?? 'P05');
  }, [eventPort]);

  const canFocusDemo =
    focusDemo &&
    affectedPorts.every((id) => demoPorts.has(id)) &&
    selected.every((id) => id === 'R04' || id === 'R06');
  const demo = canFocusDemo && !showAll && !current;
  const visiblePorts = demo ? ports.filter((port) => demoPorts.has(port.port_id)) : ports;
  const outgoing = routes.filter((route) => route.from_port_id === portId);
  const port = ports.find((item) => item.port_id === portId);

  const style = (route) =>
    updates.has(route.route_id)
      ? 'search-update'
      : !step && selected.includes(route.route_id)
      ? 'chosen'
      : affected.includes(route.route_id)
      ? 'affected'
      : route.from_port_id === portId
      ? 'focused'
      : 'available';

  const draw = order.flatMap((kind) =>
    routes
      .filter(
        (route) =>
          style(route) === kind && (!demo || (demoPorts.has(route.from_port_id) && demoPorts.has(route.to_port_id))),
      )
      .map((route) => ({ route, kind })),
  );

  const activeEvent = report?.events?.[0] ?? null;
  const activeImpact = report?.impacts?.[0] ?? null;
  const portName = (id) => ports.find((p) => p.port_id === id)?.port_name ?? id;

  const weight = (value) => search.objective === 'cheapest' ? `${cost(value)} per container` : `${value} days`;
  return (
    <>
    {search && (
      <section ref={controls} id="dijkstra-playback" className="panel search-playback" aria-label="Dijkstra playback">
        <div className="search-playback-controls">
          <h2>Dijkstra search</h2>
          <button type="button" disabled={!steps.length} onClick={() => {
            if (current?.playing) changeStep(current.index);
            else {
              changeStep(current && !finished ? current.index : 0, true);
              controls.current?.scrollIntoView({ block: 'start', behavior: 'instant' });
            }
          }}>{current?.playing ? 'Pause' : finished ? 'Replay search' : current ? 'Resume' : 'Play search'}</button>
          <button type="button" disabled={!current || current.index === 0}
            onClick={() => changeStep(current.index - 1)}>Previous step</button>
          <button type="button" disabled={!steps.length || finished}
            onClick={() => changeStep(current ? current.index + 1 : 0)}>Next step</button>
        </div>
        <div aria-live="polite" className="detail tabular">
          {step ? (
            <>
              <strong>Step {current.index + 1} of {steps.length}</strong>
              {' · '}Leg {step.segment} toward {portName(step.target_port_id)}
              {' · '}Selected {portName(step.port_id)} with the smallest tentative {search.objective === 'cheapest' ? 'cost' : 'time'} ({weight(step.weight)}).
              {step.updates.length ? (
                <div>Improved estimates: {step.updates.map((item) =>
                  `${portName(item.port_id)} via ${item.route_id} (${weight(item.weight)})`).join(' · ')}</div>
              ) : <div>{step.port_id === step.target_port_id ? 'Leg destination reached.' : 'No better estimates from this port.'}</div>}
            </>
          ) : finished ? (
            search.noRoute ? 'Search complete. No route is available.' :
              `Search complete. ${search.routeIds.join(' → ') || 'Same port'} · ${search.totalDays} days · ${cost(search.freight)}.`
          ) : steps.length ? 'Play or step through the ports and route updates recorded by the C backend.' :
            'No ports were searched. Check the required ports and closures below.'}
        </div>
        {current && <p className="detail search-key">Yellow port: current · Blue ports: settled · Cyan routes: improved estimates · Red ports: closed</p>}
      </section>
    )}
    <div className="globe-workspace">
      <div className="overview-header-overlay">
        <h1>
          {activeImpact
            ? `Shipment ${activeImpact.shipment_id} · Journey Route Interface`
            : 'Transport Network · Global Route View'}
        </h1>
      </div>

      {canFocusDemo && !current && (
        <div style={{ position: 'absolute', top: '1.25rem', right: 'clamp(1rem, 3vw, 2.2rem)', zIndex: 15 }}>
          <button
            className="compact"
            type="button"
            aria-pressed={showAll}
            onClick={() => {
              setShowAll(!showAll);
              if (showAll) setPortId('P05');
            }}
          >
            {showAll ? 'Focus on demo' : 'Show all ports'}
          </button>
        </div>
      )}

      <ShippingGlobe
        ports={visiblePorts}
        routes={draw}
        portId={portId}
        selectPort={setPortId}
        searchFocus={step?.port_id ?? (finished && !search.noRoute ? search.destination : null)}
        searchSettled={settled}
        searchClosed={closedPorts}
        searchActive={Boolean(current)}
        searchStep={step ? current.index : null}
        demo={demo}
        eventPort={eventPort}
        affectedPorts={affectedPorts}
        activeEvent={activeEvent}
        onEditDisruption={onEditDisruption}
      />

      {/* Bottom Compact Itinerary Strip */}
      <div className="globe-bottom-strip tabular">
        {activeImpact ? (
          <>
            <strong>
              {activeImpact.shipment_id} · {activeImpact.contract_id}:
            </strong>
            {activeImpact.calls.map((call, idx) => (
              <React.Fragment key={`${call.port_id}-${idx}`}>
                {idx > 0 && <span className="detail">→</span>}
                <span style={call.wait_days > 0 ? { color: '#FFB45E', fontWeight: 650 } : undefined}>
                  {portName(call.port_id)} ({formatShortDate(call.call_date)}
                  {call.wait_days > 0 ? ` / ${formatShortDate(addDays(call.call_date, call.wait_days))}` : ''})
                </span>
              </React.Fragment>
            ))}
            <span className="detail">→</span>
            <span>
              ETA {formatShortDate(activeImpact.baseline_eta)} / {formatShortDate(activeImpact.revised_eta)}
            </span>
          </>
        ) : (
          <>
            <strong>{port ? `Departures from ${port.port_name}` : 'Select a port'}:</strong>
            {outgoing.length ? (
              outgoing.slice(0, 4).map((route) => (
                <span key={route.route_id} className="detail">
                  {route.route_id} → {portName(route.to_port_id)} ({route.transit_days}d · {cost(route.cost_inr_per_container)})
                </span>
              ))
            ) : (
              <span className="detail">No outgoing routes.</span>
            )}
          </>
        )}
      </div>
    </div>
    </>
  );
}

export function RouteExplorer({ ports, onPath }) {
  const [query, setQuery] = useState({
    origin: 'P03',
    destination: 'P11',
    objective: 'fastest',
    quantity_units: 10,
    units_per_container: 10,
    required_stops: [],
    closed_ports: [],
    closed_routes: [],
  });
  const [message, setMessage] = useState('');
  const [pending, setPending] = useState(false);
  const [hasSearch, setHasSearch] = useState(false);

  const set = (key) => (event) => setQuery({ ...query, [key]: event.target.value });
  const number = (key) => (event) =>
    setQuery({ ...query, [key]: event.target.value === '' ? '' : Number(event.target.value) });
  const ids = (key) => (event) =>
    setQuery({
      ...query,
      [key]: event.target.value
        .split(',')
        .map((id) => id.trim().toUpperCase())
        .filter(Boolean),
    });

  return (
    <div className="route-lookup-wrap">
      <form
        className="panel"
        onSubmit={async (event) => {
          event.preventDefault();
          setPending(true);
          setMessage('');
          setHasSearch(false);
          onPath([]);
          try {
            const result = await api('/route', { ...query, include_trace: true });
            setMessage(
              result.no_route_reason
                ? 'No route. ' + result.no_route_reason
                : (result.route_ids.join(', ') || 'Same port') +
                    ' · ' +
                    result.total_days +
                    ' days · ' +
                    cost(result.freight_inr),
            );
            setHasSearch(Boolean(result.search_trace));
            onPath(result.route_ids, {
              steps: result.search_trace ?? [],
              routeIds: result.route_ids,
              destination: query.destination,
              objective: query.objective,
              closedPorts: query.closed_ports,
              noRoute: result.no_route_reason,
              totalDays: result.total_days,
              freight: result.freight_inr,
            });
          } catch (error) {
            setMessage(error.message);
          } finally {
            setPending(false);
          }
        }}
      >
        <h2 style={{ marginBottom: '0.85rem' }}>Route lookup</h2>
        <div className="route-fields">
          {['origin', 'destination'].map((key) => (
            <Field key={key} title={key === 'origin' ? 'Origin' : 'Destination'}>
              <select value={query[key]} onChange={set(key)}>
                {ports.map((port) => (
                  <option key={port.port_id} value={port.port_id}>
                    {port.port_name} ({port.port_id})
                  </option>
                ))}
              </select>
            </Field>
          ))}
          <Field title="Objective">
            <select value={query.objective} onChange={set('objective')}>
              <option value="fastest">fastest</option>
              <option value="cheapest">cheapest</option>
            </select>
          </Field>
        </div>
        <div className="route-fields">
          <Field title="Quantity · units">
            <input required type="number" min="1" value={query.quantity_units} onChange={number('quantity_units')} />
          </Field>
          <Field title="Units per container">
            <input required type="number" min="1" value={query.units_per_container} onChange={number('units_per_container')} />
          </Field>
        </div>
        <details className="route-assumptions" style={{ marginBottom: '0.85rem' }}>
          <summary className="detail">Required stops and closures</summary>
          <div className="route-fields" style={{ marginTop: '0.65rem' }}>
            <Field title="Required port IDs in order">
              <input placeholder="P05, P11" onChange={ids('required_stops')} />
            </Field>
            <Field title="Closed port IDs">
              <input placeholder="P05" onChange={ids('closed_ports')} />
            </Field>
            <Field title="Closed route IDs">
              <input placeholder="R04" onChange={ids('closed_routes')} />
            </Field>
          </div>
        </details>
        <div style={{ display: 'flex', alignItems: 'center', gap: '1rem', flexWrap: 'wrap' }}>
          <button className="primary" disabled={pending} type="submit">
            Find route
          </button>
          <p role="status" className="detail tabular">
            {pending ? 'Finding route…' : message}
          </p>
          {hasSearch && <a href="#dijkstra-playback">Watch the search on the globe</a>}
        </div>
      </form>
    </div>
  );
}
