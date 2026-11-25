import React, { useEffect, useId, useLayoutEffect, useRef, useState } from 'react';
import { geoDistance, geoInterpolate, geoGraticule10, geoOrthographic, geoPath } from 'd3-geo';
import { feature } from 'topojson-client';
import world from 'world-atlas/land-110m.json';
import { label } from './client.js';
import { addDays, coordinates, formatRange, formatShortDate } from './geo.js';

const home = { longitude: 31, latitude: -12, zoom: 1.05 };
const sphere = { type: 'Sphere' };
const land = feature(world, world.objects.land);
const grid = geoGraticule10();
const clamp = (value, min, max) => Math.max(min, Math.min(max, value));

export function ShippingGlobe({
  ports,
  routes,
  portId,
  selectPort,
  demo,
  eventPort,
  affectedPorts,
  activeEvent,
  onEditDisruption,
  searchFocus,
  searchSettled,
  searchClosed,
  searchActive,
  searchStep,
}) {
  const [view, setView] = useState(home);
  const [showPortPicker, setShowPortPicker] = useState(false);

  useLayoutEffect(() => {
    const location = coordinates[eventPort];
    setView((current) =>
      location ? { ...current, longitude: location[0], latitude: location[1] } : home,
    );
  }, [eventPort]);

  const [size, setSize] = useState([960, 560]);
  const surface = useRef(null);
  const liveView = useRef(view);
  liveView.current = view;
  useEffect(() => {
    const target = coordinates[searchFocus];
    if (!target) return;
    const from = [liveView.current.longitude, liveView.current.latitude];
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      setView((previous) => ({ ...previous, longitude: target[0], latitude: target[1] }));
      return;
    }
    const interpolate = geoInterpolate(from, target);
    const start = performance.now();
    let frame;
    function pan(now) {
      const time = clamp((now - start) / 330, 0, 1);
      const [longitude, latitude] = interpolate(1 - (1 - time) ** 3);
      setView((previous) => ({ ...previous, longitude, latitude }));
      if (time < 1) frame = requestAnimationFrame(pan);
    }
    frame = requestAnimationFrame(pan);
    return () => cancelAnimationFrame(frame);
  }, [searchFocus]);
  const pointers = useRef(new Map());
  const ocean = useId();
  const direction = ocean + '-direction';
  const [width, height] = size;
  const radius = Math.max(1, Math.min(width, height) / 2 - 22);
  const projection = geoOrthographic()
    .rotate([-view.longitude, -view.latitude])
    .translate([width / 2, height / 2])
    .scale(radius * view.zoom)
    .clipAngle(90)
    .precision(0.7);
  const path = geoPath(projection);
  const center = [view.longitude, view.latitude];
  const visible = (position) => geoDistance(center, position) < Math.PI / 2;

  const adjustZoom = (factor) =>
    setView((current) => ({
      ...current,
      zoom: clamp(current.zoom * factor, 0.75, 4),
    }));

  const rotate = (x, y) =>
    setView((current) => ({
      ...current,
      longitude: (((current.longitude + x + 180) % 360) + 360) % 360 - 180,
      latitude: clamp(current.latitude + y, -89, 89),
    }));

  useEffect(() => {
    const element = surface.current;
    if (!element) return;
    const observer = new ResizeObserver(([entry]) => {
      if (entry.contentRect.width > 0 && entry.contentRect.height > 0) {
        setSize([entry.contentRect.width, entry.contentRect.height]);
      }
    });
    function wheel(event) {
      if (document.activeElement !== element && !event.ctrlKey) return;
      event.preventDefault();
      adjustZoom(event.deltaY < 0 ? 1.12 : 1 / 1.12);
    }
    observer.observe(element);
    element.addEventListener('wheel', wheel, { passive: false });
    return () => {
      observer.disconnect();
      element.removeEventListener('wheel', wheel);
    };
  }, []);

  function focusPort(id) {
    selectPort(id);
    if (!coordinates[id]) return;
    const [longitude, latitude] = coordinates[id];
    setView({ longitude, latitude, zoom: view.zoom });
  }

  function drag(event) {
    const active = pointers.current;
    const previous = active.get(event.pointerId);
    if (!previous) return;
    const current = [event.clientX, event.clientY];
    const other = [...active].find(([id]) => id !== event.pointerId)?.[1];
    active.set(event.pointerId, current);
    if (other) {
      const before = Math.hypot(previous[0] - other[0], previous[1] - other[1]);
      const after = Math.hypot(current[0] - other[0], current[1] - other[1]);
      if (before > 2 && after > 2) adjustZoom(after / before);
    } else {
      const degrees = 57.3 / (radius * view.zoom);
      rotate((previous[0] - current[0]) * degrees, (current[1] - previous[1]) * degrees);
    }
  }

  function keyboard(event) {
    if (event.target !== surface.current) return;
    switch (event.key) {
      case 'ArrowLeft': rotate(-12, 0); break;
      case 'ArrowRight': rotate(12, 0); break;
      case 'ArrowUp': rotate(0, 12); break;
      case 'ArrowDown': rotate(0, -12); break;
      case '+':
      case '=': adjustZoom(1.25); break;
      case '-': adjustZoom(0.8); break;
      case 'Home': setView(home); break;
      default: return;
    }
    event.preventDefault();
  }

  const drawablePorts = ports.filter((port) => coordinates[port.port_id]);
  const selectedPort = ports.find((p) => p.port_id === portId);
  const selectedCoord = portId && coordinates[portId];
  const selectedVisible = selectedCoord && visible(selectedCoord);
  const selectedPoint = selectedVisible ? projection(selectedCoord) : null;
  const isPortDisrupted = activeEvent && activeEvent.port_id === portId;

  return (
    <div className="globe-frame">
      <div
        ref={surface}
        className="globe-surface"
        tabIndex={0}
        role="region"
        aria-label="Rotatable shipping globe"
        onKeyDown={keyboard}
        onPointerDown={(event) => {
          if (event.button !== 0 || event.target.closest('button, [role="button"], .globe-popover')) return;
          if (event.pointerType === 'mouse') event.currentTarget.focus({ preventScroll: true });
          pointers.current.set(event.pointerId, [event.clientX, event.clientY]);
          event.currentTarget.setPointerCapture(event.pointerId);
        }}
        onPointerMove={drag}
        onLostPointerCapture={(event) => pointers.current.delete(event.pointerId)}
      >
        <svg viewBox={`0 0 ${width} ${height}`} aria-label="World map and shipping routes">
          <defs>
            <radialGradient id={ocean} cx="36%" cy="28%" r="74%">
              <stop offset="0%" className="globe-ocean-light" />
              <stop offset="100%" className="globe-ocean-dark" />
            </radialGradient>
            <marker id={direction} viewBox="0 0 10 10" refX="14" refY="5" markerWidth="7" markerHeight="7" markerUnits="userSpaceOnUse" orient="auto">
              <path d="M0 0L10 5L0 10Z" fill="context-stroke" />
            </marker>
          </defs>
          <path className="globe-ocean" fill={`url(#${ocean})`} d={path(sphere)} />
          <path className="globe-land" d={path(land)} />
          <path className="globe-grid" d={path(grid)} />

          {routes.map(({ route, kind }) => {
            const start = coordinates[route.from_port_id];
            const end = coordinates[route.to_port_id];
            const line = start && end && path({ type: 'LineString', coordinates: [start, end] });
            return (
              line && (
                <g key={route.route_id}>
                  <path
                    d={line}
                    className={'edge ' + kind}
                    markerEnd={visible(end) ? `url(#${direction})` : undefined}
                  />
                  {kind === 'search-update' && searchStep != null && (
                    <g key={searchStep} className="search-line-motion" aria-hidden="true">
                      <path d={line} pathLength="1" className="voyage-trail search-line-trail" />
                      <circle r="4.5" className="voyage-beacon search-line-beacon"
                        style={{ offsetPath: `path("${line}")` }} />
                    </g>
                  )}
                </g>
              )
            );
          })}

          {drawablePorts
            .filter((port) => visible(coordinates[port.port_id]))
            .map((port) => {
              const id = port.port_id;
              const [x, y] = projection(coordinates[id]);
              if (x < 9 || x > width - 9 || y < 9 || y > height - 9) return null;
              const selected = portId === id;
              const searchClass = id === searchFocus ? ' search-current'
                : searchClosed?.includes(id) ? ' search-closed'
                : searchSettled?.has(id) ? ' search-settled' : '';
              const affected = affectedPorts.includes(id);
              const showLabel = demo || selected || (affected && width > 420);
              const anchor = x < 85 ? 'start' : x > width - 85 ? 'end' : 'middle';
              return (
                <g
                  key={id}
                  transform={`translate(${x},${y})`}
                  role="button"
                  tabIndex={0}
                  aria-label={port.port_name}
                  aria-pressed={selected}
                  className={`globe-port${selected ? ' active' : ''}${affected ? ' affected' : ''}${searchClass}`}
                  onClick={() => selectPort(id)}
                  onKeyDown={(event) => {
                    if (event.key === 'Enter' || event.key === ' ') {
                      event.preventDefault();
                      selectPort(id);
                    }
                  }}
                >
                  <title>{port.port_name}</title>
                  {(affected || selected || id === searchFocus) && <circle className="globe-port-ring" r={15} />}
                  <circle className="globe-port-hit" r={12} />
                  <circle className="globe-port-dot" r={selected ? 6 : 4.5} />
                  {showLabel && (
                    <text y={y < height - 27 ? 24 : -14} textAnchor={anchor}>
                      {port.port_name}
                    </text>
                  )}
                </g>
              );
            })}
        </svg>

        {/* Anchored Selected Port Popover */}
        {selectedPort && !searchActive && (
          <div
            className={`globe-popover${isPortDisrupted ? ' disrupted' : ''}`}
            style={
              selectedPoint
                ? {
                    left: `${clamp(selectedPoint[0] - 150, 16, Math.max(16, width - 330))}px`,
                    top: `${clamp(selectedPoint[1] - 185, 76, Math.max(76, height - 210))}px`,
                  }
                : { left: '1.5rem', top: '5.5rem' }
            }
          >
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', gap: '0.5rem' }}>
              <strong style={{ fontSize: '1.08rem', color: isPortDisrupted ? '#FFB45E' : '#F4F1E8' }}>
                {selectedPort.port_name} · {selectedPort.port_id}
              </strong>
              <span className="detail">{selectedPort.country}</span>
            </div>
            {isPortDisrupted ? (
              <>
                <p className="detail tabular" style={{ color: '#F4F1E8' }}>
                  Port {activeEvent.port_status}:{' '}
                  {formatRange(activeEvent.start, addDays(activeEvent.start, Math.max(0, activeEvent.duration_days - 1)))}{' '}
                  ({label(activeEvent.event_type)})
                </p>
                <p className="detail tabular">
                  {activeEvent.port_status === 'closed' ? 'Reopens' : 'Restriction ends'}: {formatShortDate(addDays(activeEvent.start, activeEvent.duration_days))}
                </p>
              </>
            ) : (
              <p className="detail">Port status: Available · Click another port or use Go to port.</p>
            )}

            {showPortPicker && (
              <label className="field" style={{ marginTop: '0.25rem' }}>
                <span>Go to port</span>
                <select
                  value={drawablePorts.some((p) => p.port_id === portId) ? portId : ''}
                  onChange={(e) => {
                    focusPort(e.target.value);
                    setShowPortPicker(false);
                  }}
                >
                  <option value="" disabled>
                    Choose a port
                  </option>
                  {drawablePorts.map((p) => (
                    <option key={p.port_id} value={p.port_id}>
                      {p.port_name} ({p.port_id})
                    </option>
                  ))}
                </select>
              </label>
            )}

            <div className="globe-popover-actions">
              <button type="button" onClick={() => onEditDisruption(selectedPort.port_id)}>
                Edit disruption
              </button>
              <button type="button" onClick={() => setShowPortPicker((v) => !v)}>
                Go to port
              </button>
            </div>
          </div>
        )}

        {/* Controls & Legend Dock */}
        <div className="globe-controls-dock">
          <div className="globe-buttons">
            <button type="button" aria-label="Zoom in" disabled={view.zoom >= 4} onClick={() => adjustZoom(1.25)}>
              +
            </button>
            <button type="button" aria-label="Zoom out" disabled={view.zoom <= 0.75} onClick={() => adjustZoom(0.8)}>
              −
            </button>
            <button type="button" onClick={() => setView(home)}>
              Reset view
            </button>
          </div>
          <div className="globe-legend" aria-label="Route legend">
            <span><i className="legend-dot available" /> Available</span>
            <span><i className="legend-dot disrupted" /> Disrupted</span>
            <span><i className="legend-dot active" /> Active route</span>
          </div>
        </div>
      </div>
    </div>
  );
}
