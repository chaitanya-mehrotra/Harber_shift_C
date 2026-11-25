import React, { useEffect, useState } from 'react';
import { api } from './client.js';
import { formatFullDate } from './geo.js';

export function portCalls(legs, routes) {
  const byId = new Map(routes.map((route) => [route.route_id, route]));
  const stops = [];
  legs.forEach((leg, index) => {
    const route = byId.get(leg.route_id);
    if (!route) return;
    const previous = stops.at(-1);
    const departure = { port: route.from_port_id, date: leg.planned_departure_date, role: 'Departure' };
    if (previous?.port === departure.port && previous.date === departure.date) {
      previous.role = 'Transfer';
    } else {
      stops.push(departure);
    }
    stops.push({
      port: route.to_port_id,
      date: leg.planned_arrival_date,
      role: index === legs.length - 1 ? 'Arrival' : 'Transfer',
    });
  });
  return stops;
}

export function Itinerary({ shipmentId, ports, routes, planEvent }) {
  const [open, setOpen] = useState(false);
  const [result, setResult] = useState(null);
  const [retry, setRetry] = useState(0);

  useEffect(() => {
    if (!open) return;
    let active = true;
    api('/shipments/' + encodeURIComponent(shipmentId) + '/itinerary').then(
      (legs) => {
        if (active) setResult({ legs });
      },
      (issue) => {
        if (active) setResult({ error: issue.message });
      },
    );
    return () => {
      active = false;
    };
  }, [open, shipmentId, retry]);

  const stops = result?.legs && portCalls(result.legs, routes);
  return (
    <details className="shipment-itinerary" onToggle={(event) => setOpen(event.currentTarget.open)}>
      <summary>Shipment itinerary</summary>
      {open &&
        (!result ? (
          <p role="status">Loading itinerary…</p>
        ) : result.error ? (
          <div className="notice" role="alert">
            {result.error}
            <button
              type="button"
              onClick={() => {
                setResult(null);
                setRetry(retry + 1);
              }}
            >
              Retry
            </button>
          </div>
        ) : !stops.length ? (
          <p className="detail">No scheduled port calls.</p>
        ) : (
          <ol className="itinerary-stops">
            {stops.map((stop, index) => {
              const name = ports.find(({ port_id }) => port_id === stop.port)?.port_name ?? stop.port;
              return (
                <li key={index}>
                  <strong>{name}</strong>
                  <span className="detail">
                    {stop.role} · <time dateTime={stop.date}>{stop.date}</time>
                  </span>
                  <button
                    type="button"
                    className="text-button"
                    onClick={() => planEvent(stop)}
                    aria-label={`Simulate disruption here: ${name}, ${stop.date}`}
                  >
                    Simulate disruption here
                  </button>
                </li>
              );
            })}
          </ol>
        ))}
    </details>
  );
}

export function ShipmentsBrowser({
  report,
  shipments,
  ports,
  routes,
  form,
  onSelectDataset,
  onSelectShipment,
  onSimulateStop,
}) {
  const [selectedId, setSelectedId] = useState(
    () => form.shipment_id || shipments[0]?.shipment?.shipment_id || '',
  );
  const [itinerary, setItinerary] = useState({ id: null, legs: null, error: '' });
  const [highlightedStop, setHighlightedStop] = useState(null);

  const activeId = form.shipment_id || selectedId || shipments[0]?.shipment?.shipment_id;
  const activeRecord = shipments.find(({ shipment }) => shipment.shipment_id === activeId) ?? shipments[0];
  const portName = (id) => ports.find((p) => p.port_id === id)?.port_name ?? id;

  useEffect(() => {
    if (form.manufacturing || !activeRecord) return;
    let alive = true;
    const id = activeRecord.shipment.shipment_id;
    api('/shipments/' + encodeURIComponent(id) + '/itinerary').then(
      (legs) => {
        if (alive) setItinerary({ id, legs, error: '' });
      },
      (issue) => {
        if (alive) setItinerary({ id, legs: null, error: issue.message });
      },
    );
    return () => {
      alive = false;
    };
  }, [activeRecord, form.manufacturing]);

  const stops =
    !form.manufacturing && itinerary.id === activeRecord?.shipment?.shipment_id && itinerary.legs
      ? portCalls(itinerary.legs, routes)
      : [];

  return (
    <div className="shipments-layout">
      <aside className="panel shipments-sidebar" aria-label="Shipments list">
        <h2>Shipments</h2>
        <label className="field">
          <span>Dataset</span>
          <select
            value={form.manufacturing ? 'demo' : 'sample'}
            onChange={(e) => onSelectDataset(e.target.value === 'demo')}
          >
            <option value="sample">Sample shipments</option>
            <option value="demo">Manufacturing demo</option>
          </select>
        </label>

        {form.manufacturing ? (
          <div className="shipment-cards">
            <button type="button" className="shipment-card" aria-pressed="true">
              <div className="shipment-card-id tabular">DEMO-RAW · DEMO-CUSTOMER</div>
              <div className="shipment-card-route">Savannah → Durban → Mumbai</div>
              <div className="shipment-card-meta tabular">
                3 stops · Deadline {formatFullDate(report.customer_deadline)}
              </div>
            </button>
          </div>
        ) : (
          <div className="shipment-cards">
            {shipments.map(({ shipment, contract }) => {
              const isSelected = shipment.shipment_id === activeRecord?.shipment?.shipment_id;
              return (
                <button
                  key={shipment.shipment_id}
                  type="button"
                  className="shipment-card"
                  aria-pressed={isSelected}
                  onClick={() => {
                    setSelectedId(shipment.shipment_id);
                    setHighlightedStop(null);
                    onSelectShipment(shipment.shipment_id);
                  }}
                >
                  <div className="shipment-card-id tabular">
                    {shipment.shipment_id} · {contract.contract_id}
                  </div>
                  <div className="shipment-card-route">
                    {portName(shipment.origin_port_id)} → {portName(shipment.destination_port_id)}
                  </div>
                  <div className="shipment-card-meta tabular">
                    {shipment.quantity_units} units · Deadline {formatFullDate(contract.deadline)}
                  </div>
                </button>
              );
            })}
          </div>
        )}
      </aside>

      <section className="panel itinerary-panel" aria-label="Shipment itinerary detail">
        {form.manufacturing ? (
          <>
            <div className="itinerary-header">
              <h1>Manufacturing Demo · Contract DEMO-CUSTOMER</h1>
              <p className="detail tabular">
                Original Itinerary · Customer deadline: {formatFullDate(report.customer_deadline)}
              </p>
            </div>
            {report.cascade.map((task, idx) => (
              <div key={task.task_id} className={`stop-card${idx === 0 ? ' highlighted' : ''}`}>
                <div className="stop-card-main">
                  <div className="stop-card-title">
                    <span>{task.label}</span>
                    <span className="badge neutral">{task.task_id}</span>
                  </div>
                  <div className="stop-card-date tabular">
                    Planned: {formatFullDate(task.baseline.start)} → {formatFullDate(task.baseline.finish)}
                  </div>
                </div>
                {task.task_id === 'DEMO-RAW' && (
                  <button
                    type="button"
                    className="amber-btn"
                    onClick={() => onSimulateStop({ port: 'P05', date: task.baseline.start })}
                  >
                    Simulate disruption at Durban
                  </button>
                )}
              </div>
            ))}
          </>
        ) : activeRecord ? (
          <>
            <div className="itinerary-header">
              <h1>
                Shipment {activeRecord.shipment.shipment_id} · Contract {activeRecord.contract.contract_id}
              </h1>
              <p className="detail tabular">
                Original Itinerary · Customer deadline: {formatFullDate(activeRecord.contract.deadline)}
              </p>
            </div>

            {itinerary.error && (
              <div className="notice" role="alert">
                {itinerary.error}
              </div>
            )}

            {!itinerary.error && itinerary.id !== activeRecord.shipment.shipment_id && (
              <p className="loading" role="status">
                Loading itinerary…
              </p>
            )}

            {stops.map((stop, idx) => {
              const isHighlighted = highlightedStop === idx || (highlightedStop === null && stop.role === 'Transfer');
              return (
                <div
                  key={`${stop.port}-${idx}`}
                  className={`stop-card${isHighlighted ? ' highlighted' : ''}`}
                  onMouseEnter={() => setHighlightedStop(idx)}
                >
                  <div className="stop-card-main">
                    <div className="stop-card-title">
                      <span>
                        Stop {idx + 1} · {portName(stop.port)} <span className="detail">({stop.port})</span>
                      </span>
                      <span className={`badge ${stop.role === 'Transfer' ? 'amber' : 'neutral'}`}>{stop.role}</span>
                    </div>
                    <div className="stop-card-date tabular">Planned date: {formatFullDate(stop.date)}</div>
                  </div>
                  <button
                    type="button"
                    className={isHighlighted ? 'amber-btn' : ''}
                    onClick={() => onSimulateStop(stop)}
                    aria-label={`Simulate disruption here: ${portName(stop.port)}, ${stop.date}`}
                  >
                    Simulate disruption here
                  </button>
                </div>
              );
            })}
          </>
        ) : (
          <p className="detail">Select a shipment to inspect its itinerary.</p>
        )}
      </section>
    </div>
  );
}
