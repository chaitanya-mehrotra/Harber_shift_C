import React, { useEffect, useState } from 'react';
import { api, cost, label } from './client.js';
import { addDays, daysBetween, formatFullDate, formatRange, formatShortDate } from './geo.js';

const days = (value) => `${value} ${value === 1 ? 'day' : 'days'}`;
const predictionNumber = new Intl.NumberFormat('en-IN', { maximumFractionDigits: 2 });

function deliveryOutcome(report, record) {
  if (report.manufacturing) {
    const delivery = report.cascade.find((task) => task.task_id === 'delivery');
    if (!delivery) return null;
    return {
      planned: delivery.baseline.finish,
      revised: delivery.revised.finish,
      deadline: report.customer_deadline,
      penalty: report.estimated_penalties_inr,
    };
  }
  if (!record) return null;
  const { shipment, contract } = record;
  const impact = report.impacts.find((item) => item.shipment_id === shipment.shipment_id);
  const revised = impact?.revised_eta ?? shipment.planned_arrival_date;
  return {
    planned: shipment.planned_arrival_date,
    revised,
    deadline: contract.deadline,
    penalty: Math.max(0, daysBetween(revised, contract.deadline)) * contract.late_penalty_inr_per_day,
  };
}

export function DelayAnalysis({
  report,
  query,
  shipments,
  ports,
  pending,
  onAdjustInputs,
  onCompareRecovery,
}) {
  const [open, setOpen] = useState(true);
  const [shipmentId, setShipmentId] = useState('');
  const [alternateResult, setAlternateResult] = useState(null);

  useEffect(() => {
    if (!open || pending || !query || !report.events.length) return;
    const controller = new AbortController();
    api('/simulate', { ...query, use_ml: !query.use_ml }, controller.signal).then(
      (alternate) => {
        if (!controller.signal.aborted) setAlternateResult({ id: report.scenario_id, alternate });
      },
      (issue) => {
        if (!controller.signal.aborted) setAlternateResult({ id: report.scenario_id, error: issue.message });
      },
    );
    return () => controller.abort();
  }, [open, pending, query, report.scenario_id, report.events.length]);

  if (!query || !report.events.length) return null;

  const portName = (id) => ports.find((p) => p.port_id === id)?.port_name ?? id;
  const touched = report.impacts.filter((impact) => impact.calls.some((call) => call.status !== 'already completed'));
  const impact = touched.find((item) => item.shipment_id === shipmentId) ?? touched[0];
  const record = shipments.find(({ shipment }) => shipment.shipment_id === (impact?.shipment_id ?? query.shipment_id));

  const currentAlt = alternateResult?.id === report.scenario_id ? alternateResult : null;
  const ruleReport = query.use_ml ? currentAlt?.alternate : report;
  const mlReport = query.use_ml ? report : currentAlt?.alternate;

  const ruleDelivery = ruleReport && deliveryOutcome(ruleReport, record);
  const mlDelivery = mlReport && deliveryOutcome(mlReport, record);

  const activeEvent = report.events[0];
  const mlImpact = mlReport?.impacts.find((item) => item.shipment_id === impact?.shipment_id);
  const estimate = mlImpact?.calls.flatMap((call) => call.model_estimates)
    .find((item) => item.event_id === activeEvent.event_id);
  const inputs = estimate?.inputs;

  return (
    <section className="panel estimate-panel" aria-label="Delay estimation breakdown">
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '0.75rem' }}>
        <div>
          <h2 style={{ fontSize: '1.35rem' }}>
            {impact ? `${impact.shipment_id} · ${impact.contract_id} · ` : ''}
            {portName(activeEvent.port_id)} {label(activeEvent.event_type)} (
            {formatRange(activeEvent.start, addDays(activeEvent.start, Math.max(0, activeEvent.duration_days - 1)))})
          </h2>
          <p className="detail">How the delay was estimated</p>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.6rem' }}>
          <span className="badge amber">PORT STATUS: {activeEvent.port_status.toUpperCase()}</span>
          <button type="button" onClick={() => setOpen((v) => !v)}>
            {open ? 'Hide details' : 'Show details'}
          </button>
        </div>
      </div>

      {open && (
        <>
          {touched.length > 1 && (
            <label className="field" style={{ maxWidth: '16rem' }}>
              <span>Shipment to inspect</span>
              <select value={impact?.shipment_id ?? ''} onChange={(e) => setShipmentId(e.target.value)}>
                {touched.map((item) => (
                  <option key={item.shipment_id} value={item.shipment_id}>
                    {item.shipment_id} · {item.contract_id}
                  </option>
                ))}
              </select>
            </label>
          )}

          {inputs && (
            <div>
              <div className="detail" style={{ marginBottom: '0.45rem' }}>ML model inputs</div>
              <div className="ml-pills tabular">
                <span className="ml-pill">Port: <strong>{portName(inputs.port_id)}</strong></span>
                <span className="ml-pill">Event: <strong>{label(inputs.event_type)}</strong></span>
                <span className="ml-pill">Duration: <strong>{days(inputs.expected_duration_days)}</strong></span>
                <span className="ml-pill">Severity: <strong>{inputs.severity_1_to_5}/5</strong></span>
                <span className="ml-pill">Congestion: <strong>{inputs.congestion_0_to_5}</strong></span>
                <span className="ml-pill">Planned transit: <strong>{days(inputs.planned_transit_days)}</strong></span>
                <span className="ml-pill">Schedule buffer: <strong>{days(inputs.schedule_buffer_days)}</strong></span>
              </div>
            </div>
          )}
          {currentAlt?.error && <p role="alert">{currentAlt.error}</p>}

          {/* 3-Column Comparison */}
          <div className="estimate-columns tabular">
            {/* Column 1: Original plan */}
            <article className="estimate-col">
              <h3>Original plan</h3>
              {ruleDelivery ? (
                <>
                  <p style={{ color: 'var(--cyan)', fontWeight: 650 }}>
                    Arrival: {formatFullDate(ruleDelivery.planned)}
                  </p>
                  <p className="detail">Delay: 0 days</p>
                  <p>Penalty: {cost(0)}</p>
                </>
              ) : (
                <>
                  <p>Affected shipments: 0</p>
                  <p className="detail">Contracts at risk: 0</p>
                  <p>Penalty: {cost(0)}</p>
                </>
              )}
            </article>

            {/* Column 2: Rule-based */}
            <article className="estimate-col">
              <h3>Rule-based</h3>
              {!ruleReport ? (
                <p className="detail">{currentAlt?.error ? 'Rule-based estimate unavailable.' : 'Loading rule-based estimate…'}</p>
              ) : ruleDelivery ? (
                <>
                  <p>
                    Delay: <strong>{days(Math.max(0, daysBetween(ruleDelivery.revised, ruleDelivery.planned)))}</strong>
                  </p>
                  <p>Arrival: {formatFullDate(ruleDelivery.revised)}</p>
                  {ruleDelivery.deadline && (
                    <div>
                      {daysBetween(ruleDelivery.revised, ruleDelivery.deadline) > 0 ? (
                        <span className="badge red">
                          {days(daysBetween(ruleDelivery.revised, ruleDelivery.deadline))} past{' '}
                          {formatShortDate(ruleDelivery.deadline)} deadline
                        </span>
                      ) : (
                        <span className="badge cyan">
                          Meets {formatShortDate(ruleDelivery.deadline)} deadline
                        </span>
                      )}
                    </div>
                  )}
                  <p>Penalty: {cost(ruleDelivery.penalty)}</p>
                </>
              ) : (
                <>
                  <p>Affected shipments: {ruleReport.affected_shipment_ids.length}</p>
                  <p>Contracts at risk: {ruleReport.urgent_contract_ids.length}</p>
                  <p>Penalty: {cost(ruleReport.estimated_penalties_inr)}</p>
                </>
              )}
            </article>

            {/* Column 3: Synthetic ML */}
            <article
              className={`estimate-col ml-highlight${
                mlDelivery && mlDelivery.deadline && daysBetween(mlDelivery.revised, mlDelivery.deadline) <= 0
                  ? ' on-time'
                  : ''
              }`}
            >
              <h3>Synthetic ML</h3>
              {!mlReport ? (
                <p className="detail">{currentAlt?.error ? 'ML estimate unavailable.' : 'Comparing ML estimate…'}</p>
              ) : mlDelivery ? (
                <>
                  {estimate ? (
                    <>
                      <p className="detail">
                        Raw prediction: {predictionNumber.format(estimate.predicted_days)} days
                      </p>
                      <p className="detail">Rounded wait: {days(estimate.rounded_days)}</p>
                    </>
                  ) : (
                    <p className="detail">
                      {activeEvent.port_status === 'closed'
                        ? 'Closed ports use the reopening date.'
                        : 'No model prediction for this shipment and event.'}
                    </p>
                  )}
                  <p style={{ fontSize: '1.05rem', fontWeight: 700 }}>
                    Arrival: {formatFullDate(mlDelivery.revised)}
                  </p>
                  {mlDelivery.deadline && (
                    <div>
                      {daysBetween(mlDelivery.revised, mlDelivery.deadline) > 0 ? (
                        <span className="badge red">
                          {days(daysBetween(mlDelivery.revised, mlDelivery.deadline))} past{' '}
                          {formatShortDate(mlDelivery.deadline)} deadline
                        </span>
                      ) : (
                        <span className="badge cyan">
                          Meets {formatShortDate(mlDelivery.deadline)} deadline
                        </span>
                      )}
                    </div>
                  )}
                  <p style={{ color: 'var(--amber)', fontWeight: 700 }}>
                    Estimated penalty: {cost(mlDelivery.penalty)}
                  </p>
                </>
              ) : (
                <>
                  <p>Affected shipments: {mlReport.affected_shipment_ids.length}</p>
                  <p>Contracts at risk: {mlReport.urgent_contract_ids.length}</p>
                  <p style={{ color: 'var(--amber)', fontWeight: 700 }}>
                    Estimated penalty: {cost(mlReport.estimated_penalties_inr)}
                  </p>
                </>
              )}
            </article>
          </div>

          <div className="estimate-footer">
            {ruleDelivery?.deadline && (
              <span className="detail tabular" style={{ marginRight: 'auto' }}>
                CUSTOMER DEADLINE: {formatFullDate(ruleDelivery.deadline)}
              </span>
            )}
            <button type="button" onClick={onAdjustInputs}>
              Adjust Inputs
            </button>
            <button type="button" className="primary" onClick={onCompareRecovery}>
              Compare recovery options →
            </button>
          </div>
        </>
      )}
    </section>
  );
}
