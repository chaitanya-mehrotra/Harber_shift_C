import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { api, label } from './client.js';
import { Network, RouteExplorer } from './network.jsx';
import { Controls, initial } from './controls.jsx';
import { Recovery } from './results.jsx';
import { ShipmentsBrowser } from './itinerary.jsx';
import { OverviewView } from './overview.jsx';
import { Advisories } from './advisories.jsx';
import { DelayAnalysis } from './delay-analysis.jsx';
import './style.css';
import './theme.css';

const views = ['Overview', 'Network', 'Shipments', 'Disruptions', 'Recovery'];

function App() {
  useLayoutEffect(() => {
    document.documentElement.dataset.theme = 'dark';
  }, []);

  const [started, setStarted] = useState(false);
  const [view, setView] = useState('Overview');
  const [form, setForm] = useState(initial);
  const [data, setData] = useState(null);
  const [report, setReport] = useState(null);
  const [appliedQuery, setAppliedQuery] = useState(null);
  const [pending, setPending] = useState(true);
  const [error, setError] = useState('');
  const [selected, setSelected] = useState(null);
  const [path, setPath] = useState([]);
  const [search, setSearch] = useState(null);
  const [replay, setReplay] = useState(false);
  const [revision, setRevision] = useState(0);
  const controlsRef = useRef(null);
  const timer = useRef();
  const request = useRef(null);

  function cancelSimulation() {
    clearTimeout(timer.current);
    request.current?.abort();
  }

  useEffect(() => cancelSimulation, []);

  useEffect(() => {
    if (!started) return;
    let active = true;
    Promise.all([
      api('/ports'),
      api('/routes'),
      api('/shipments'),
      api('/disruptions'),
      api('/simulate', { as_of: initial.as_of, manufacturing: initial.manufacturing }),
    ])
      .then(([ports, routes, shipments, events, initialReport]) => {
        if (active) {
          setData({ ports, routes, shipments, events });
          setReport(initialReport);
          setAppliedQuery(null);
        }
      })
      .catch((issue) => {
        if (active) setError(issue.message);
      })
      .finally(() => {
        if (active) setPending(false);
      });
    return () => {
      active = false;
    };
  }, [revision, started]);

  function editScenario(next, fromAdvisory = replay && next.sample === 'manual') {
    cancelSimulation();
    setForm(next);
    setReplay(fromAdvisory);
    setPending(true);
    setError('');
    timer.current = setTimeout(() => {
      const invalid = controlsRef.current?.querySelector(':invalid');
      if (invalid) {
        const name = invalid.labels?.[0]?.querySelector('span')?.textContent;
        setError([name, invalid.validationMessage].filter(Boolean).join('. '));
        setPending(false);
        return;
      }
      simulate(true, next, fromAdvisory);
    }, 250);
  }

  async function simulate(apply, draft = form, fromAdvisory = replay) {
    cancelSimulation();
    const controller = new AbortController();
    request.current = controller;
    setPending(true);
    setError('');
    const query = {
      as_of: draft.as_of || report?.as_of || initial.as_of,
      manufacturing: draft.manufacturing,
      shipment_id: draft.shipment_id || null,
      units_per_container: apply ? draft.units_per_container : undefined,
      use_ml: apply && draft.use_ml,
      congestion:
        apply && Number.isInteger(draft.congestion) && draft.congestion >= 0 && draft.congestion <= 5
          ? draft.congestion
          : 0,
      buffer_days: apply && Number.isInteger(draft.buffer_days) && draft.buffer_days >= 0 ? draft.buffer_days : 0,
      original_purchase_inr: apply && !draft.manufacturing && draft.shipment_id ? draft.purchase || null : null,
    };
    if (apply && draft.sample !== 'manual') query.sample_ids = [draft.sample];
    if (apply && draft.sample === 'manual') {
      query.event = {
        start: draft.start || null,
        port_id: draft.port_id,
        event_type: draft.event_type,
        duration_days: draft.duration_days,
        severity: draft.severity,
        port_status: draft.port_status,
        advisory_replay: fromAdvisory,
      };
    }
    try {
      const result = await api('/simulate', query, controller.signal);
      if (controller.signal.aborted) return;
      setReport(result);
      setAppliedQuery(apply ? query : null);
      setSelected(null);
      setPath([]);
      setSearch(null);
      if (!apply) setReplay(false);
    } catch (issue) {
      if (!controller.signal.aborted) setError(issue.message);
    } finally {
      if (!controller.signal.aborted) setPending(false);
    }
  }

  function toggleModel(checked) {
    const next = { ...form, use_ml: checked };
    if (appliedQuery) editScenario(next);
    else setForm(next);
  }

  function goToDisruptionForPort(portId, date) {
    if (portId) {
      setForm((prev) => ({
        ...prev,
        sample: 'manual',
        port_id: portId,
        start: date || prev.start,
      }));
    }
    setView('Disruptions');
  }

  const estimateLabel = report?.prediction?.sources
    ? report.prediction.sources
        .map((source) => (source === 'rule_fallback' ? 'Rule-based estimate' : source === 'synthetic_model' ? 'Synthetic ML' : label(source)))
        .join(' + ')
    : 'Rule-based estimate';

  const isFullBleed = view === 'Overview' || view === 'Network';

  if (!started) {
    return (
      <main className="welcome-screen">
        <div className="welcome-water" aria-hidden="true" />
        <section className="welcome-content" aria-labelledby="welcome-title">
          <span className="welcome-anchor" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
              <circle cx="12" cy="5" r="2.2" />
              <path d="M12 7.2V21M8 11h8M5 14a7 7 0 0 0 14 0" />
            </svg>
          </span>
          <h1 id="welcome-title">HarborShift</h1>
          <p>Explore a shipping network and see how port disruptions change delivery schedules.</p>
          <button className="primary welcome-start" type="button" onClick={() => setStarted(true)}>
            Start simulation <span aria-hidden="true">→</span>
          </button>
        </section>
      </main>
    );
  }

  return (
    <>
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round" strokeLinejoin="round">
              <circle cx="12" cy="5" r="2.2" />
              <path d="M12 7.2V21M8 11h8M5 14a7 7 0 0 0 14 0" />
            </svg>
          </span>
          <div className="brand-text">
            <span className="brand-title">HarborShift</span>
            <span className="brand-subtitle">
              DISRUPTION SIMULATOR · {form.manufacturing ? 'DEMO MODE' : 'SAMPLE SHIPMENTS'}
            </span>
          </div>
        </div>

        <nav aria-label="Main navigation">
          {views.map((mode) => (
            <button key={mode} aria-current={view === mode ? 'page' : undefined} onClick={() => setView(mode)}>
              {mode}
            </button>
          ))}
        </nav>

        <div className="header-actions">
          <label className="ml-switch" title="Toggle Synthetic ML delay estimation for restricted ports">
            <input
              type="checkbox"
              checked={form.use_ml}
              onChange={(e) => toggleModel(e.target.checked)}
            />
            <span className="switch-track" aria-hidden="true" />
            <span>Synthetic data</span>
          </label>
          <span className={`model-tag${form.use_ml ? ' ml-active' : ''}`}>{estimateLabel}</span>
        </div>
      </header>

      <main className={isFullBleed ? 'full-bleed' : ''}>
        {error && !data && (
          <div className="notice" role="alert" style={{ margin: '1.5rem' }}>
            {error}{' '}
            <button
              onClick={() => {
                setPending(true);
                setError('');
                setRevision(revision + 1);
              }}
            >
              Retry
            </button>
          </div>
        )}
        {pending && !data && (
          <p className="loading" role="status" style={{ margin: '1.5rem' }}>
            Loading HarborShift…
          </p>
        )}

        {data && report && (
          <>
            {view === 'Overview' && (
              <OverviewView
                report={report}
                ports={data.ports}
                routes={data.routes}
                shipments={data.shipments}
                form={form}
                onEditDisruption={(portId) => goToDisruptionForPort(portId)}
                onCompareRecovery={() => setView('Recovery')}
                onOpenNetwork={() => setView('Network')}
                onOpenDisruptions={() => setView('Disruptions')}
                onToggleMl={toggleModel}
              />
            )}

            {view === 'Network' && (
              <>
                <Network
                  search={search}
                  ports={data.ports}
                  routes={data.routes}
                  affected={report.affected_edge_ids}
                  affectedPorts={report.affected_port_ids}
                  selected={selected?.route_ids ?? path}
                  focusDemo={report.manufacturing}
                  report={report}
                  onEditDisruption={(portId) => goToDisruptionForPort(portId)}
                />
                <RouteExplorer
                  ports={data.ports}
                  onPath={(ids, searchResult = null) => {
                    setSelected(null);
                    setPath(ids);
                    setSearch(searchResult);
                  }}
                />
              </>
            )}

            {view === 'Shipments' && (
              <ShipmentsBrowser
                report={report}
                shipments={data.shipments}
                ports={data.ports}
                routes={data.routes}
                form={form}
                onSelectDataset={(manufacturing) =>
                  editScenario({ ...form, manufacturing, shipment_id: '', purchase: '' })
                }
                onSelectShipment={(shipment_id) =>
                  editScenario({ ...form, manufacturing: false, shipment_id, purchase: '' })
                }
                onSimulateStop={(stop) => {
                  editScenario({
                    ...form,
                    sample: 'manual',
                    port_id: stop.port,
                    start: stop.date,
                  });
                  setView('Disruptions');
                }}
              />
            )}

            {view === 'Disruptions' && (
              <div>
                {replay && (
                  <p className="notice">
                    Advisory replay. Review the port and event type, then enter your own severity and duration.
                  </p>
                )}
                <Controls
                  controlsRef={controlsRef}
                  form={form}
                  setForm={editScenario}
                  {...data}
                  report={report}
                  pending={pending}
                  error={error}
                  apply={() => simulate(true)}
                  reset={() => simulate(false)}
                />
                <DelayAnalysis
                  report={report}
                  query={appliedQuery}
                  shipments={data.shipments}
                  ports={data.ports}
                  pending={pending}
                  onAdjustInputs={() => controlsRef.current?.querySelector('input, select')?.focus()}
                  onCompareRecovery={() => setView('Recovery')}
                />
                <Advisories
                  events={data.events}
                  report={report}
                  replay={(candidate) => {
                    editScenario(
                      {
                        ...form,
                        sample: 'manual',
                        port_id: candidate.port_id,
                        event_type: candidate.event_type,
                        start: '',
                        duration_days: '',
                        severity: '',
                        port_status: 'restricted',
                      },
                      true,
                    );
                  }}
                />
              </div>
            )}

            {view === 'Recovery' && (
              <Recovery
                report={report}
                ports={data.ports}
                selected={selected}
                select={setSelected}
                onGoToDisruptions={() => setView('Disruptions')}
              />
            )}

            {!isFullBleed && report.warnings?.length > 0 && (
              <details className="warnings">
                <summary>Scenario notes ({report.warnings.length})</summary>
                <ul>
                  {report.warnings.map((warning) => (
                    <li key={warning}>{warning}</li>
                  ))}
                </ul>
              </details>
            )}
          </>
        )}
      </main>
    </>
  );
}

createRoot(document.getElementById('root')).render(<App />);
