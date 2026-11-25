import React, { useEffect, useId, useMemo, useRef, useState } from 'react';
import { geoGraticule10, geoNaturalEarth1, geoPath } from 'd3-geo';
import { feature } from 'topojson-client';
import world from 'world-atlas/land-110m.json';
import { api, cost, label } from './client.js';
import { addDays, coordinates, daysBetween, formatFullDate, formatRange, formatShortDate } from './geo.js';

const land = feature(world, world.objects.land);
const grid = geoGraticule10();

function deriveStopsFromLegs(legs, routes, impact) {
  const byId = new Map(routes.map((route) => [route.route_id, route]));
  const stops = [];
  let previousArrival;
  for (const leg of legs) {
    const route = byId.get(leg.route_id);
    if (!route) continue;
    const scheduledDeparture = leg.planned_departure_date;
    const scheduledArrival = leg.planned_arrival_date;
    const departure = previousArrival && previousArrival > scheduledDeparture ? previousArrival : scheduledDeparture;
    const departureCall = impact?.calls.find((call) =>
      call.leg_sequence === leg.leg_sequence && call.call_type === 'departure');
    const revisedDeparture = addDays(departure, departureCall?.wait_days ?? 0);
    const sailingDays = daysBetween(scheduledArrival, scheduledDeparture);
    const arrivalCall = impact?.calls.find((call) =>
      call.leg_sequence === leg.leg_sequence && call.call_type === 'arrival');
    previousArrival = addDays(revisedDeparture, sailingDays + (arrivalCall?.wait_days ?? 0));

    const previousStop = stops.at(-1);
    if (previousStop?.port_id === route.from_port_id && previousStop.planned_date === scheduledDeparture) {
      previousStop.revised_date = revisedDeparture;
      previousStop.disrupted ||= Boolean(departureCall?.wait_days);
    } else {
      stops.push({
        port_id: route.from_port_id,
        planned_date: scheduledDeparture,
        revised_date: revisedDeparture,
        disrupted: Boolean(departureCall?.wait_days),
      });
    }
    stops.push({
      port_id: route.to_port_id,
      planned_date: scheduledArrival,
      revised_date: previousArrival,
      disrupted: Boolean(arrivalCall?.wait_days),
    });
  }
  return stops;
}

export function OverviewView({
  report,
  ports,
  routes,
  shipments,
  form,
  onEditDisruption,
  onCompareRecovery,
  onOpenNetwork,
  onOpenDisruptions,
  onToggleMl,
}) {
  const mapRef = useRef(null);
  const [size, setSize] = useState([1280, 620]);
  const [zoom, setZoom] = useState(1);
  const [introRun, setIntroRun] = useState(0);
  const [showAllPorts, setShowAllPorts] = useState(false);
  const [itineraryLegs, setItineraryLegs] = useState(null);

  const oceanId = useId();
  const arrowId = `${oceanId}-arrow`;

  const portMap = useMemo(() => new Map(ports.map((p) => [p.port_id, p])), [ports]);
  const activeImpact = useMemo(() => {
    if (form.shipment_id) {
      return report.impacts.find((i) => i.shipment_id === form.shipment_id);
    }
    const urgent = report.impacts.find((i) => i.contract_id === report.urgent_contract_ids[0]);
    return urgent ?? report.impacts[0];
  }, [report, form.shipment_id]);

  const activeShipmentId = activeImpact?.shipment_id
    ?? (report.manufacturing ? 'DEMO-RAW' : (form.shipment_id || shipments[0]?.shipment?.shipment_id));

  const activeRecord = useMemo(
    () => shipments.find(({ shipment }) => shipment.shipment_id === activeShipmentId),
    [shipments, activeShipmentId],
  );

  useEffect(() => {
    if (!activeShipmentId || report.manufacturing) {
      setItineraryLegs(null);
      return;
    }
    let alive = true;
    api('/shipments/' + encodeURIComponent(activeShipmentId) + '/itinerary')
      .then((legs) => { if (alive) setItineraryLegs({ shipmentId: activeShipmentId, legs }); })
      .catch(() => { if (alive) setItineraryLegs(null); });
    return () => { alive = false; };
  }, [activeShipmentId, report.manufacturing]);

  useEffect(() => {
    const el = mapRef.current;
    if (!el) return;
    const observer = new ResizeObserver(([entry]) => {
      if (entry.contentRect.width > 0 && entry.contentRect.height > 0) {
        setSize([entry.contentRect.width, entry.contentRect.height]);
      }
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, []);

  const stops = useMemo(() => {
    if (report.manufacturing) {
      const stages = [
        { taskId: 'raw', port: 'P03', role: 'departure' },
        { taskId: 'raw', port: 'P05', role: 'arrival', caption: 'arrival' },
        { taskId: 'finished', port: 'P05', role: 'departure', caption: 'departure' },
        { taskId: 'finished', port: 'P11', role: 'arrival' },
      ];
      return stages.map(({ taskId, port, role, caption }) => {
        const task = report.cascade.find((item) => item.task_id === taskId);
        const call = task.calls.find((item) => item.call_type === role && item.port_id === port);
        const timing = role === 'arrival' ? 'finish' : 'start';
        return {
          port_id: port,
          caption,
          planned_date: task.baseline[timing],
          revised_date: role === 'departure' && call
            ? addDays(call.call_date, call.wait_days)
            : task.revised[timing],
          disrupted: Boolean(call?.wait_days),
        };
      });
    }
    if (itineraryLegs?.shipmentId === activeShipmentId) {
      return deriveStopsFromLegs(itineraryLegs.legs, routes, activeImpact);
    }
    if (activeImpact?.calls?.length) {
      return activeImpact.calls.map((c, idx) => ({
        port_id: c.port_id,
        planned_date: c.call_date,
        revised_date: addDays(c.call_date, c.wait_days),
        wait_days: c.wait_days,
        role: idx === 0 ? 'Departure' : idx === activeImpact.calls.length - 1 ? 'Arrival' : 'Transfer',
        route_id: c.route_id,
        disrupted: c.wait_days > 0 || (c.event_ids?.length ?? 0) > 0,
      }));
    }
    return [];
  }, [itineraryLegs, activeShipmentId, routes, activeImpact, report]);

  const [width, height] = size;
  const projection = useMemo(() => {
    const base = geoNaturalEarth1().fitExtent([[48, 56], [width - 48, height - 42]], { type: 'Sphere' });
    if (zoom === 1) return base;
    return base.scale(base.scale() * zoom).translate([width / 2, height / 2]);
  }, [width, height, zoom]);
  const pathGen = useMemo(() => geoPath(projection), [projection]);

  const activeEvent = report.events[0] ?? null;
  const disruptedPortId = activeEvent?.port_id ?? report.affected_port_ids[0] ?? null;
  const disruptedCoords = disruptedPortId && coordinates[disruptedPortId] ? projection(coordinates[disruptedPortId]) : null;

  const deliveryTask = report.manufacturing ? report.cascade.find((c) => c.task_id === 'delivery') : null;
  const destinationStop = stops.at(-1);
  const destPortId = report.manufacturing ? 'P11' : (activeRecord?.shipment?.destination_port_id ?? destinationStop?.port_id);
  const destPort = portMap.get(destPortId);

  const plannedEta = deliveryTask?.baseline?.finish ?? activeImpact?.baseline_eta ?? activeRecord?.shipment?.planned_arrival_date ?? destinationStop?.planned_date;
  const revisedEta = deliveryTask?.revised?.finish ?? activeImpact?.revised_eta ?? destinationStop?.revised_date ?? plannedEta;
  const deadline = report.customer_deadline ?? activeImpact?.deadline ?? activeRecord?.contract?.deadline;
  const delayDays = plannedEta && revisedEta ? Math.max(0, daysBetween(revisedEta, plannedEta)) : 0;
  const pastDeadlineDays = deadline && revisedEta ? daysBetween(revisedEta, deadline) : 0;
  const penaltyInr = report.manufacturing
    ? report.estimated_penalties_inr
    : Math.max(0, pastDeadlineDays) * (activeRecord?.contract?.late_penalty_inr_per_day ?? activeImpact?.penalty_per_day ?? 0);

  const contractLabel = report.manufacturing ? 'DEMO-CUSTOMER' : (activeRecord?.contract?.contract_id ?? activeImpact?.contract_id ?? '');
  const titleLabel = report.manufacturing
    ? 'Manufacturing Demo · Journey Route Interface'
    : `Shipment ${activeShipmentId || 'Overview'} · Journey Route Interface`;

  const visiblePorts = showAllPorts
    ? ports.filter((p) => coordinates[p.port_id])
    : ports.filter((p) => stops.some((s) => s.port_id === p.port_id) || p.port_id === disruptedPortId);

  const showIntro = report.manufacturing && report.events.length === 0;
  return (
    <div className="overview-stage">
      <section className="overview-summary" aria-labelledby="overview-summary-title">
        <div className="overview-summary-header">
          <div>
            <h1 id="overview-summary-title">{report.manufacturing ? 'Manufacturing overview' : 'Shipment overview'}</h1>
            <p className="detail">{report.manufacturing ? 'Savannah → Durban → Mumbai' : activeShipmentId || 'Sample shipments'}</p>
          </div>
          <div className="overview-summary-actions">
            <button className="primary" type="button" onClick={onOpenDisruptions}>Simulate a disruption</button>
            <button type="button" onClick={onOpenNetwork}>Explore network</button>
            {showIntro && <button className="voyage-replay" type="button" onClick={() => {
              setIntroRun((run) => run + 1);
              mapRef.current?.scrollIntoView({ block: 'nearest' });
            }}>Replay voyage</button>}
          </div>
        </div>
        {report.manufacturing && (
          <ol className="overview-milestones" aria-label="Manufacturing milestones">
            {report.cascade.map((task) => {
              const changed = task.revised.finish !== task.baseline.finish;
              return (
                <li key={task.task_id} className={changed ? 'milestone-delayed' : undefined}>
                  <span>{task.label}</span>
                  <strong>{formatShortDate(task.revised.finish)}</strong>
                  <small>{changed ? `Planned ${formatShortDate(task.baseline.finish)}` : 'On schedule'}</small>
                </li>
              );
            })}
          </ol>
        )}
      </section>
      <div className="overview-map-wrap">
        <div className="overview-header-overlay">
          <h2>{report.manufacturing ? 'Voyage map' : titleLabel}</h2>
        </div>

        <svg ref={mapRef} className="overview-map-svg" viewBox={`0 0 ${width} ${height}`} aria-label="Shipment journey route map">
          <defs>
            <radialGradient id={oceanId} cx="45%" cy="42%" r="72%">
              <stop offset="0%" stopColor="#132c4a" />
              <stop offset="65%" stopColor="#0b1b30" />
              <stop offset="100%" stopColor="#060f1c" />
            </radialGradient>
            <marker id={arrowId} viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" markerHeight="6" orient="auto">
              <path d="M0 1L10 5L0 9Z" fill="#61DBC4" />
            </marker>
          </defs>

          <rect width={width} height={height} fill={`url(#${oceanId})`} />
          <path d={pathGen(grid)} fill="none" stroke="rgba(130, 170, 210, 0.08)" strokeWidth="0.7" />
          <path d={pathGen(land)} fill="#1a2f3d" stroke="#35586b" strokeWidth="0.85" />

          {/* Background shipping lanes */}
          {routes.map((r) => {
            const a = coordinates[r.from_port_id];
            const b = coordinates[r.to_port_id];
            if (!a || !b) return null;
            const d = pathGen({ type: 'LineString', coordinates: [a, b] });
            return d ? (
              <path key={r.route_id} d={d} fill="none" stroke="rgba(156, 176, 196, 0.14)" strokeWidth="1" />
            ) : null;
          })}

          {/* Active shipment route arcs */}
          {stops.slice(0, -1).map((stop, idx) => {
            const next = stops[idx + 1];
            if (stop.port_id === next.port_id) return null;
            const a = coordinates[stop.port_id];
            const b = coordinates[next.port_id];
            if (!a || !b) return null;
            const d = pathGen({ type: 'LineString', coordinates: [a, b] });
            const isPostDisruption = stops.slice(0, idx + 1).some((s) => s.disrupted);
            return d ? (
              <g key={`${stop.port_id}-${next.port_id}`}>
                <path
                  d={d}
                  fill="none"
                  stroke={isPostDisruption ? '#FFB45E' : '#61DBC4'}
                  strokeWidth="2.6"
                  strokeDasharray={isPostDisruption ? '6 5' : undefined}
                  markerEnd={`url(#${arrowId})`}
                />
                {showIntro && (
                  <g key={introRun} className="voyage-motion" aria-hidden="true">
                    <path className="voyage-trail" d={d} pathLength="1"
                      style={{ animationDelay: `${idx * 1.1}s` }} />
                    <circle className="voyage-beacon" r="4"
                      style={{ offsetPath: `path("${d}")`, animationDelay: `${idx * 1.1}s` }} />
                  </g>
                )}
              </g>
            ) : null;
          })}

          {/* Visible port markers */}
          {visiblePorts.map((port) => {
            const coord = coordinates[port.port_id];
            if (!coord) return null;
            const [cx, cy] = projection(coord);
            const stop = stops.find((s) => s.port_id === port.port_id);
            const isDisrupted = port.port_id === disruptedPortId && report.events.length > 0;
            const isOnRoute = Boolean(stop);
            return (
              <g key={port.port_id} transform={`translate(${cx}, ${cy})`}>
                {isDisrupted && (
                  <>
                    <circle r="18" fill="rgba(255, 180, 94, 0.18)" stroke="#FFB45E" strokeWidth="2" />
                    <circle r="26" fill="none" stroke="rgba(255, 180, 94, 0.4)" strokeWidth="1.2" />
                  </>
                )}
                <circle
                  r={isDisrupted ? 6.5 : isOnRoute ? 5.5 : 3.5}
                  fill={isDisrupted ? '#FFB45E' : isOnRoute ? '#61DBC4' : '#7ea2b8'}
                  stroke="#091321"
                  strokeWidth="2"
                />
                {(isOnRoute || isDisrupted) && (
                  <g transform="translate(0, 18)">
                    <text
                      textAnchor="middle"
                      fill="#F4F1E8"
                      fontSize="12.5"
                      fontWeight="700"
                      stroke="#091321"
                      strokeWidth="3.5"
                      paintOrder="stroke"
                    >
                      {port.port_name}
                    </text>
                    {stop && (
                      <text
                        y="14"
                        textAnchor="middle"
                        fill={stop.revised_date !== stop.planned_date ? '#FFB45E' : '#9CB0C4'}
                        fontSize="11"
                        fontWeight="600"
                        stroke="#091321"
                        strokeWidth="3"
                        paintOrder="stroke"
                      >
                        {formatShortDate(stop.revised_date)}
                        {stop.revised_date !== stop.planned_date ? ' (revised)' : ''}
                      </text>
                    )}
                  </g>
                )}
              </g>
            );
          })}
        </svg>

        {/* Anchored Disruption Card */}
        {activeEvent && disruptedPortId && (
          <div
            className="overview-disruption-card"
            style={
              disruptedCoords
                ? {
                    left: `${Math.min(Math.max(disruptedCoords[0] - 140, 16), Math.max(16, width - 340))}px`,
                    top: `${Math.min(Math.max(disruptedCoords[1] - 175, 72), Math.max(72, height - 190))}px`,
                  }
                : { left: '2rem', top: '5.5rem' }
            }
          >
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', gap: '0.5rem' }}>
              <strong style={{ fontSize: '1.05rem', color: '#F4F1E8' }}>
                {portMap.get(disruptedPortId)?.port_name ?? disruptedPortId}
              </strong>
              <span className="detail tabular">({disruptedPortId})</span>
            </div>
            <div className="disruption-tag">Disruption Active</div>
            <p className="detail tabular" style={{ color: '#F4F1E8' }}>
              Port {activeEvent.port_status}:{' '}
              {formatRange(activeEvent.start, addDays(activeEvent.start, Math.max(0, activeEvent.duration_days - 1)))} (
              {label(activeEvent.event_type)}, {activeEvent.duration_days}d)
            </p>
            <button type="button" onClick={() => onEditDisruption(disruptedPortId)}>
              Edit disruption
            </button>
          </div>
        )}

        {/* Destination Result Card */}
        <aside className="overview-result-card" aria-label="Destination arrival outcome">
          <div className="result-dest">
            Destination: <strong>{destPort?.port_name ?? destPortId ?? 'Destination'}</strong>{' '}
            {destPortId && <span className="tabular">({destPortId})</span>}
          </div>
          <div className="result-arrival tabular">Arrival: {formatFullDate(revisedEta)}</div>
          <div className="detail tabular">
            {delayDays > 0
              ? `Delayed from ${formatShortDate(plannedEta)} (${delayDays} ${delayDays === 1 ? 'day' : 'days'} delayed)`
              : `On schedule · Planned ${formatShortDate(plannedEta)}`}
          </div>
          {deadline && (
            <div>
              {pastDeadlineDays > 0 ? (
                <span className="badge red tabular">
                  Missed {formatShortDate(deadline)} Deadline · {pastDeadlineDays} {pastDeadlineDays === 1 ? 'day' : 'days'} past
                </span>
              ) : (
                <span className="badge cyan tabular">Meets {formatShortDate(deadline)} Deadline</span>
              )}
            </div>
          )}
          <div className="result-penalty tabular">Penalty: {cost(penaltyInr)}</div>
          <button type="button" className="primary" onClick={onCompareRecovery}>
            Compare recovery options →
          </button>
        </aside>

        {/* Map Controls */}
        <div className="overview-map-controls">
          <span className="detail">Zoom</span>
          <button type="button" aria-label="Zoom out" onClick={() => setZoom((z) => Math.max(0.85, +(z / 1.2).toFixed(2)))}>
            −
          </button>
          <button type="button" aria-label="Zoom in" onClick={() => setZoom((z) => Math.min(2.6, +(z * 1.2).toFixed(2)))}>
            +
          </button>
          <button type="button" onClick={() => setZoom(1)}>
            Reset
          </button>
          <button type="button" aria-pressed={showAllPorts} onClick={() => setShowAllPorts((v) => !v)}>
            {showAllPorts ? 'Route ports' : 'All ports'}
          </button>
          <label className="ml-switch" style={{ marginLeft: '0.25rem' }}>
            <input type="checkbox" checked={form.use_ml} onChange={(e) => onToggleMl(e.target.checked)} />
            <span className="switch-track" aria-hidden="true" />
            <span>ML</span>
          </label>
        </div>
      </div>

      {/* Bottom Dual-Track Voyage Tracker */}
      <section className="voyage-tracker" aria-label="Voyage tracker">
        <div>
          <div className="tracker-meta-title">Voyage Tracker</div>
          <div className="tracker-meta-sub tabular">
            {report.manufacturing ? contractLabel : [activeShipmentId, contractLabel].filter(Boolean).join(' · ')}
          </div>
        </div>
        <div className="tracker-tracks">
          {/* PLANNED */}
          <div className="tracker-row">
            <span className="tracker-row-label">Planned</span>
            <div className="tracker-line-wrap">
              <div className="tracker-line-bar" />
              {stops.map((stop, idx) => (
                <div className="tracker-stop" key={`plan-${stop.port_id}-${idx}`}>
                  <span className="tracker-stop-name">{portMap.get(stop.port_id)?.port_name ?? stop.port_id}{stop.caption && ` · ${stop.caption}`}</span>
                  <span className="tracker-stop-dot" />
                  <span className="tracker-stop-date tabular">{formatShortDate(stop.planned_date)}</span>
                </div>
              ))}
              <div className="detail tabular" style={{ fontSize: '0.74rem', zIndex: 2, paddingLeft: '0.5rem' }}>
                Planned arrival
              </div>
            </div>
          </div>

          {/* REVISED */}
          <div className="tracker-row">
            <span className="tracker-row-label">Revised</span>
            <div className="tracker-line-wrap">
              <div className="tracker-line-bar revised" />
              {stops.map((stop, idx) => (
                <React.Fragment key={`rev-${stop.port_id}-${idx}`}>
                  <div className="tracker-stop">
                    <span className="tracker-stop-name">{portMap.get(stop.port_id)?.port_name ?? stop.port_id}{stop.caption && ` · ${stop.caption}`}</span>
                    <span className={`tracker-stop-dot${stop.disrupted ? ' disrupted' : ''}`} />
                    <span
                      className="tracker-stop-date tabular"
                      style={stop.revised_date !== stop.planned_date ? { color: '#FFB45E' } : undefined}
                    >
                      {formatShortDate(stop.revised_date)}
                    </span>
                  </div>
                  {stop.disrupted && activeEvent && (
                    <span className="tracker-closure-pill tabular">
                      {formatShortDate(activeEvent.start)}–{formatShortDate(addDays(activeEvent.start, Math.max(0, activeEvent.duration_days - 1)))}{' '}
                      {activeEvent.port_status.toUpperCase()}
                    </span>
                  )}
                </React.Fragment>
              ))}
              {deadline && (
                <span className="tracker-deadline-tag tabular">
                  DEADLINE ({formatShortDate(deadline)})
                </span>
              )}
            </div>
          </div>
        </div>
      </section>
    </div>
  );
}
