import React, { useMemo, useState } from 'react';
import { eventTypes, label } from './client.js';
import { addDays, formatFullDate, formatShortDate } from './geo.js';

export const initial = {
  as_of: '2027-01-05',
  sample: 'E001',
  manufacturing: true,
  shipment_id: '',
  port_id: 'P05',
  event_type: 'strike',
  duration_days: 5,
  severity: 4,
  port_status: 'closed',
  start: '',
  purchase: '',
  units_per_container: 10,
  use_ml: false,
  congestion: 0,
  buffer_days: 0,
};

export function Field({ title, children, className = '' }) {
  return (
    <label className={`field ${className}`.trim()}>
      <span>{title}</span>
      {children}
    </label>
  );
}

function ClosureWindowPanel({ form, ports, events, report, pending }) {
  const sample = events.find((e) => e.event_id === form.sample);
  const portId = form.sample === 'manual' ? form.port_id : sample?.port_id ?? form.port_id;
  const startDate = form.sample === 'manual' ? form.start || form.as_of : sample?.reported_date ?? form.as_of;
  const durationDays =
    form.sample === 'manual'
      ? Math.max(0, Number(form.duration_days) || 0)
      : sample?.expected_duration_days ?? 0;
  const portStatus = form.sample === 'manual' ? form.port_status : sample?.port_status ?? 'closed';

  const portName = ports.find((p) => p.port_id === portId)?.port_name ?? portId;
  const reopenDate = addDays(startDate, durationDays);

  const days = useMemo(() => {
    const list = [];
    for (let offset = -2; offset <= durationDays; offset++) {
      const iso = addDays(startDate, offset);
      list.push({
        iso,
        isClosed: offset >= 0 && offset < durationDays,
        isReopen: offset === durationDays,
      });
    }
    return list;
  }, [startDate, durationDays]);

  const eventId = form.sample;
  const appliedEvent = pending ? null : report.events.find((event) =>
    event.event_id === eventId && event.port_id === portId && event.start === startDate
    && event.duration_days === durationDays && event.port_status === portStatus
    && (eventId !== 'manual' || (event.event_type === form.event_type && event.severity === form.severity)),
  );
  const overlaps = (call) => call.port_id === portId && call.event_ids.includes(eventId)
    && call.call_date >= startDate && call.call_date < reopenDate;
  const collidingImpact = appliedEvent && report.impacts.find((impact) => impact.calls.some(overlaps));
  const collidingCall = collidingImpact?.calls.find(overlaps);

  return (
    <section className="panel calendar-panel" aria-label="Closure window and schedule overlap">
      <div>
        <h2>Closure Window &amp; Schedule Overlap</h2>
        <p className="detail tabular">
          {portName} ({portId}) · {formatFullDate(startDate)}
        </p>
      </div>

      <div>
        <div className="calendar-banner">
          <span className="badge amber tabular">
            {portStatus.toUpperCase()} ({durationDays} {durationDays === 1 ? 'day' : 'days'})
          </span>
          <span className="badge cyan tabular">{portStatus === 'closed' ? 'Reopens' : 'Restriction ends'} {formatShortDate(reopenDate)}</span>
        </div>

        <div className="calendar-strip tabular">
          {days.map((d) => {
            const isHit = collidingCall && collidingCall.call_date === d.iso;
            const cls = d.isReopen
              ? 'calendar-day reopen'
              : isHit
              ? 'calendar-day closed-hit'
              : d.isClosed
              ? 'calendar-day closed'
              : 'calendar-day';
            return (
              <div key={d.iso} className={cls} title={d.iso}>
                {formatShortDate(d.iso)}
              </div>
            );
          })}
        </div>
      </div>

      <div className="overlap-callout">
        {collidingImpact && collidingCall ? (
          <>
            <strong style={{ fontSize: '0.98rem' }}>
              {collidingImpact.shipment_id} · {collidingImpact.contract_id}
            </strong>
            <span className="detail tabular">
              Scheduled {portName} {label(collidingCall.call_type)}: {formatShortDate(collidingCall.call_date)}
            </span>
            <span className="badge amber tabular" style={{ justifySelf: 'start', marginTop: '0.25rem' }}>
              {collidingCall.wait_days > 0
                ? `Overlap detected: ${collidingCall.wait_days} ${collidingCall.wait_days === 1 ? 'day' : 'days'} of wait until ${formatShortDate(
                    addDays(collidingCall.call_date, collidingCall.wait_days),
                  )}`
                : 'Overlap detected · no modeled wait'}
            </span>
          </>
        ) : (
          <>
            <strong>Schedule check</strong>
            <span className="detail">
              {pending ? 'Checking for overlapping port calls…' : appliedEvent
                ? `No scheduled port calls at ${portName} overlap ${formatShortDate(startDate)} – ${formatShortDate(
                    addDays(startDate, Math.max(0, durationDays - 1)),
                  )}.`
                : 'Apply the event to evaluate overlap against scheduled port calls.'}
            </span>
          </>
        )}
      </div>
    </section>
  );
}

export function Controls({
  controlsRef,
  form,
  setForm,
  ports,
  events,
  shipments,
  report,
  pending,
  error,
  apply,
  reset,
}) {
  const [validationError, setValidationError] = useState('');
  const set = (key) => (event) => setForm({ ...form, [key]: event.target.value });
  const number = (key) => (event) =>
    setForm({ ...form, [key]: event.target.value === '' ? '' : Number(event.target.value) });

  const isManual = form.sample === 'manual';
  const sample = events.find((event) => event.event_id === form.sample);
  const selectedShipment = shipments.find(({ shipment }) => shipment.shipment_id === form.shipment_id)?.shipment;
  const applied = report.events[0];
  const appliedPort = applied && ports.find((port) => port.port_id === applied.port_id)?.port_name;
  const affected = report.affected_shipment_ids.length;

  const shipmentSelectorValue = form.manufacturing ? 'demo' : form.shipment_id || 'all';

  function handleShipmentSelect(value) {
    if (value === 'demo') {
      setForm({ ...form, manufacturing: true, shipment_id: '', purchase: '' });
    } else if (value === 'all') {
      setForm({ ...form, manufacturing: false, shipment_id: '', purchase: '' });
    } else {
      setForm({ ...form, manufacturing: false, shipment_id: value, purchase: '' });
    }
  }

  function showInvalidField(event) {
    const field = event.target;
    if (field !== event.currentTarget.querySelector(':invalid')) return;
    const details = field.closest('details');
    if (details) details.open = true;
    const name = field.labels?.[0]?.querySelector('span')?.textContent;
    setValidationError(name ? `${name}. ${field.validationMessage}` : field.validationMessage);
  }

  const failure = validationError || error;

  return (
    <div>
      <div style={{ marginBottom: '1rem' }}>
        <h1>
          {selectedShipment
            ? `Shipment ${selectedShipment.shipment_id} · Disruption Simulator`
            : form.manufacturing
            ? 'Manufacturing Demo · Disruption Simulator'
            : 'Disruption Simulator'}
        </h1>
      </div>

      <div className="mode-tabs" role="tablist" aria-label="Disruption mode">
        <button
          type="button"
          role="tab"
          aria-selected={!isManual}
          onClick={() => {
            if (isManual) {
              const first = events[0];
              setForm({
                ...form,
                sample: first?.event_id ?? 'E001',
                as_of: first?.reported_date ?? form.as_of,
              });
            }
          }}
        >
          Preset event
        </button>
        <button
          type="button"
          role="tab"
          aria-selected={isManual}
          onClick={() => setForm({ ...form, sample: 'manual' })}
        >
          Manual event
        </button>
      </div>

      <div className="disruptions-grid">
        <form
          ref={controlsRef}
          className="panel controls-grid"
          onInvalid={showInvalidField}
          onChange={() => setValidationError('')}
          onSubmit={(event) => {
            event.preventDefault();
            setValidationError('');
            apply();
          }}
        >
          <Field title="Shipment" className="controls-span">
            <select value={shipmentSelectorValue} onChange={(e) => handleShipmentSelect(e.target.value)}>
              <option value="demo">Manufacturing demo</option>
              <option value="all">All sample shipments</option>
              {shipments.map(({ shipment }) => (
                <option key={shipment.shipment_id} value={shipment.shipment_id}>
                  {shipment.shipment_id} · {shipment.contract_id} (Sample shipments)
                </option>
              ))}
            </select>
          </Field>

          {!isManual ? (
            <>
              <Field title="Preset event" className="controls-span">
                <select
                  name="disruption"
                  value={form.sample}
                  onChange={(event) => {
                    const saved = events.find((item) => item.event_id === event.target.value);
                    setForm({ ...form, sample: event.target.value, as_of: saved?.reported_date ?? form.as_of });
                  }}
                >
                  {events.map((event) => (
                    <option key={event.event_id} value={event.event_id}>
                      {event.event_id} · {label(event.event_type)} ·{' '}
                      {ports.find((port) => port.port_id === event.port_id)?.port_name}
                    </option>
                  ))}
                </select>
              </Field>

              {sample && (
                <div className="event-preview controls-span">
                  <strong>{sample.headline.replace(/^Synthetic: /, '')}</strong>
                  <span className="detail tabular">
                    {sample.expected_duration_days} days · severity {sample.severity_1_to_5}/5 · {sample.port_status} ·
                    reported {sample.reported_date}
                  </span>
                </div>
              )}

              <Field title="Simulated date" className="controls-span">
                <input type="date" required value={form.as_of} onChange={set('as_of')} />
              </Field>
            </>
          ) : (
            <>
              <Field title="Port">
                <select value={form.port_id} onChange={set('port_id')}>
                  {ports.map((port) => (
                    <option key={port.port_id} value={port.port_id}>
                      {port.port_name} ({port.port_id})
                    </option>
                  ))}
                </select>
              </Field>
              <Field title="Event type">
                <select value={form.event_type} onChange={set('event_type')}>
                  {eventTypes.map((type) => (
                    <option key={type} value={type}>
                      {label(type)}
                    </option>
                  ))}
                </select>
              </Field>
              <Field title="Port status">
                <select value={form.port_status} onChange={set('port_status')}>
                  <option value="closed">Closed</option>
                  <option value="restricted">Restricted</option>
                </select>
              </Field>
              <Field title="Severity · 1–5">
                <input type="number" min="1" max="5" required value={form.severity} onChange={number('severity')} />
              </Field>
              <Field title="Duration · days">
                <input
                  type="number"
                  min="0"
                  required
                  value={form.duration_days}
                  onChange={number('duration_days')}
                />
              </Field>
              <Field title="Simulated date">
                <input type="date" required value={form.as_of} onChange={set('as_of')} />
              </Field>
              <Field title="Event start (optional)" className="controls-span distinct-start">
                <input
                  type="date"
                  value={form.start}
                  onChange={set('start')}
                  aria-describedby="event-start-help"
                />
                <small id="event-start-help" className="detail">
                  Leave blank to use the simulated date.
                </small>
              </Field>
            </>
          )}

          <details className="controls-span" style={{ borderTop: '1px solid var(--border-subtle)', paddingTop: '0.75rem' }}>
            <summary className="detail" style={{ fontWeight: 600 }}>
              Packing and delay assumptions
            </summary>
            <div className="controls-grid" style={{ marginTop: '0.75rem' }}>
              <Field title="Units per container">
                <input
                  type="number"
                  min="1"
                  required
                  value={form.units_per_container}
                  onChange={number('units_per_container')}
                />
              </Field>
              {selectedShipment && (
                <Field title="Original purchase cost · INR">
                  <input
                    type="number"
                    min="0"
                    step="0.01"
                    value={form.purchase}
                    onChange={set('purchase')}
                    placeholder="Unknown"
                  />
                </Field>
              )}
              <label className="check-row controls-span">
                <input
                  type="checkbox"
                  checked={form.use_ml}
                  onChange={(event) => setForm({ ...form, use_ml: event.target.checked })}
                />
                Use synthetic ML for restrictions
              </label>
              {form.use_ml && (
                <>
                  <Field title="Congestion · 0–5">
                    <input type="number" min="0" max="5" required value={form.congestion} onChange={number('congestion')} />
                  </Field>
                  <Field title="Schedule buffer · days">
                    <input type="number" min="0" required value={form.buffer_days} onChange={number('buffer_days')} />
                  </Field>
                </>
              )}
            </div>
          </details>

          <div className="actions-row controls-span">
            <button className="primary" type="submit">
              Apply event
            </button>
            <button
              type="button"
              onClick={() => {
                setValidationError('');
                reset();
              }}
            >
              Reset
            </button>
          </div>

          {failure && (
            <p className="notice controls-span" role="alert" style={{ margin: 0 }}>
              {failure}
            </p>
          )}
          {!failure && (pending || applied) && (
            <p className="detail controls-span tabular" role="status">
              {pending ? (
                'Running scenario…'
              ) : (
                <>
                  <strong>Event applied.</strong> Simulated {label(applied.event_type)} at{' '}
                  {appliedPort ?? applied.port_id}, starting {applied.start}.{' '}
                  {affected === 0
                    ? 'No shipments were affected. Check the event dates against scheduled port calls.'
                    : `${affected} ${affected === 1 ? 'shipment' : 'shipments'} affected.`}
                </>
              )}
            </p>
          )}
        </form>

        <ClosureWindowPanel form={form} ports={ports} events={events} report={report} pending={pending} />
      </div>
    </div>
  );
}
