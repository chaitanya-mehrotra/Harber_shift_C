import React, { useState } from 'react';
import { api, label } from './client.js';

export function Advisories({ report, replay }) {
  const [news, setNews] = useState(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState('');

  async function loadNews() {
    setPending(true);
    setError('');
    try {
      setNews(await api('/news'));
    } catch (issue) {
      setError(issue.message);
    } finally {
      setPending(false);
    }
  }

  return (
    <section className="panel" style={{ marginTop: '1.25rem' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '0.75rem' }}>
        <div>
          <h2>Public advisories</h2>
          <p className="detail">
            Review published items separately from the fictional shipment timeline. Replay uses the simulated date and
            requires your own duration and severity assumptions.
          </p>
        </div>
        <button type="button" onClick={loadNews} disabled={pending}>
          {pending ? 'Loading advisories…' : 'Fetch public advisories'}
        </button>
      </div>

      {report.events.length > 0 && (
        <p className="detail" style={{ marginTop: '0.5rem' }}>
          Active scenario event: {report.events.map((e) => e.event_id).join(', ')}
        </p>
      )}
      {error && (
        <p className="notice" role="alert" style={{ marginTop: '0.75rem' }}>
          {error}
        </p>
      )}
      {news?.errors.map((issue) => (
        <p className="detail" key={issue} style={{ marginTop: '0.5rem' }}>
          {issue}
        </p>
      ))}
      {news && !news.articles.length && <p className="detail" style={{ marginTop: '0.75rem' }}>No matching disruption candidates.</p>}
      {news?.articles.map((item) => (
        <article className="advisory" key={item.url}>
          <div>
            <h3>
              <a href={item.url} target="_blank" rel="noreferrer">
                {item.title}
              </a>
            </h3>
            <p className="detail">
              {item.source} · {item.published ?? 'Timestamp unknown'} · {item.candidate.confidence}
            </p>
            <p className="detail">
              {label(item.candidate.event_type ?? 'Unknown event')} · {item.candidate.port_id ?? 'Port unknown'} ·
              Duration and severity unknown
            </p>
          </div>
          <button
            type="button"
            disabled={!item.candidate.port_id || !item.candidate.event_type}
            onClick={() => replay(item.candidate)}
          >
            Review as simulated replay
          </button>
        </article>
      ))}
    </section>
  );
}
