import React from 'react';
import { cost, label } from './client.js';
import { addDays, daysBetween, formatFullDate, formatRange, formatShortDate } from './geo.js';

export function Recovery({ report, ports = [], selected, select, onGoToDisruptions }) {
  const activeEvent = report.events[0] ?? null;
  const disruptedPortName =
    (activeEvent && ports.find((p) => p.port_id === activeEvent.port_id)?.port_name) ??
    activeEvent?.port_id ??
    'Port';

  if (!report.recovery.length) {
    return (
      <section className="panel">
        <div className="recovery-header">
          <h1>Recovery Options</h1>
          <p className="detail">
            Apply a disruption event that affects a shipment to compare waiting vs. alternative sourcing or rerouting.
          </p>
        </div>
        {onGoToDisruptions && (
          <button type="button" className="primary" onClick={onGoToDisruptions}>
            Go to Disruptions →
          </button>
        )}
      </section>
    );
  }

  return (
    <div style={{ display: 'grid', gap: '1.5rem' }}>
      {report.recovery.map((item) => {
        const impact = report.impacts.find((imp) => imp.shipment_id === item.shipment_id);
        const deadline = report.customer_deadline ?? impact?.deadline;
        const feasibleOptions = item.options.filter((opt) => opt.status !== 'rejected');
        const rejectedOptions = item.options.filter((opt) => opt.status === 'rejected');

        return (
          <section key={item.shipment_id} aria-label={`Recovery options for ${item.shipment_id}`}>
            <div className="recovery-header">
              <h1>
                Recovery Options · {item.shipment_id} · Contract {item.contract_id}
              </h1>
              <p className="detail tabular">
                {activeEvent
                  ? `${disruptedPortName} ${label(activeEvent.port_status)} (${formatRange(
                      activeEvent.start,
                      addDays(activeEvent.start, Math.max(0, activeEvent.duration_days - 1)),
                    )})`
                  : 'Active disruption'}
                {deadline ? ` · Customer Deadline: ${formatFullDate(deadline)}` : ''}
              </p>
            </div>

            <div className="recovery-cards">
              {feasibleOptions.map((option, idx) => {
                const isWait = option.kind === 'wait';
                const isSupplier = option.kind === 'supplier';
                const isRecommended = option.option_id === item.recommended_id;
                const isSelected =
                  selected?.option_id === option.option_id && selected?.shipment_id === item.shipment_id;
                const lateDays = option.late_days ?? (deadline && option.eta ? Math.max(0, daysBetween(option.eta, deadline)) : 0);
                const beforeDeadlineDays =
                  deadline && option.eta ? Math.max(0, daysBetween(deadline, option.eta)) : 0;
                const baselineEta = report.manufacturing
                  ? report.cascade.find((task) => task.task_id === 'delivery')?.baseline.finish
                  : impact?.baseline_eta;
                const delayFromBaseline = baselineEta && option.eta
                  ? Math.max(0, daysBetween(option.eta, baselineEta)) : 0;

                const additionalCostDisplay = !isSupplier
                  ? cost(option.freight_inr)
                  : option.purchase_inr != null
                  ? cost(option.purchase_inr)
                  : '—';

                return (
                  <article
                    key={option.option_id}
                    className={`recovery-card${isRecommended ? ' recommended' : ''}${
                      isSelected ? ' selected-option' : ''
                    }`}
                  >
                    <div className="recovery-card-head">
                      <h2 style={{ fontSize: '1.12rem' }}>
                        Option {idx + 1}:{' '}
                        {isWait
                          ? (activeEvent?.port_status === 'restricted' ? `Wait at ${disruptedPortName}` : `Wait for ${disruptedPortName} to reopen`)
                          : option.kind === 'reroute'
                          ? `Reroute (${option.option_id})`
                          : `Use replacement supplier ${option.option_id}`}
                      </h2>
                      <span className={`badge ${lateDays === 0 ? 'cyan' : 'neutral'}`}>
                        {isWait
                          ? 'Baseline action'
                          : lateDays === 0
                          ? 'Available · Meets deadline'
                          : label(option.status)}
                      </span>
                    </div>

                    <div className="recovery-rows tabular">
                      <div className="recovery-row">
                        <span className="recovery-row-label">
                          {isSupplier ? 'Replacement delivery' : 'Original shipment arrival'}
                        </span>
                        <span
                          className="recovery-row-value"
                          style={{ color: lateDays === 0 ? 'var(--cyan)' : 'var(--amber)' }}
                        >
                          {option.eta ? formatFullDate(option.eta) : '—'}
                          {isWait && delayFromBaseline > 0 && ` (${delayFromBaseline} days delayed)`}
                          {!isWait && beforeDeadlineDays > 0 && ` (${beforeDeadlineDays} days before deadline)`}
                        </span>
                      </div>

                      <div className="recovery-row">
                        <span className="recovery-row-label">Deadline status</span>
                        <span className="recovery-row-value">
                          {lateDays > 0 ? (
                            <span className="badge red">
                              {lateDays} {lateDays === 1 ? 'day' : 'days'} past{' '}
                              {deadline ? formatShortDate(deadline) : ''} deadline
                            </span>
                          ) : (
                            <span className="badge cyan">
                              Meets {deadline ? formatShortDate(deadline) : ''} deadline
                            </span>
                          )}
                        </span>
                      </div>

                      <div className="recovery-row">
                        <span className="recovery-row-label">
                          Estimated penalty
                        </span>
                        <span
                          className="recovery-row-value"
                          style={!isWait ? { color: 'var(--text-primary)' } : { color: 'var(--amber)' }}
                        >
                          {cost(option.penalty_inr)}
                          {!isWait && lateDays === 0 && deadline ? ` (meets ${formatShortDate(deadline)} deadline)` : ''}
                        </span>
                      </div>

                      <div className="recovery-row">
                        <span className="recovery-row-label">
                          {isSupplier ? 'Supplier quote / cost' : 'Remaining freight'}
                        </span>
                        <span className="recovery-row-value">{additionalCostDisplay}</span>
                      </div>

                      <div className="recovery-row">
                        <span className="recovery-row-label">Notes</span>
                        <span className="recovery-row-value" style={{ fontWeight: 450, color: 'var(--text-muted)' }}>
                          {option.reason}
                        </span>
                      </div>
                    </div>

                    <div className="recovery-card-foot">
                      <button
                        type="button"
                        className={!isWait || isRecommended ? 'primary' : ''}
                        aria-pressed={isSelected}
                        onClick={() => select({ ...option, shipment_id: item.shipment_id })}
                      >
                        {isSelected
                          ? `Selected: ${isWait ? 'Wait' : option.option_id}`
                          : isWait
                          ? 'Select Wait'
                          : `Select ${option.option_id} →`}
                      </button>
                    </div>
                  </article>
                );
              })}
            </div>

            {rejectedOptions.length > 0 && (
              <details className="warnings" style={{ marginTop: '0.85rem' }}>
                <summary>Rejected options and reasons ({rejectedOptions.length})</summary>
                <ul style={{ marginTop: '0.5rem', paddingLeft: '1.25rem' }}>
                  {rejectedOptions.map((opt) => (
                    <li key={opt.option_id}>
                      <strong>{opt.option_id}</strong> ({label(opt.kind)}) · {opt.reason}
                    </li>
                  ))}
                </ul>
              </details>
            )}
          </section>
        );
      })}
    </div>
  );
}
