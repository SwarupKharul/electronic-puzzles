import React, { useState, useEffect } from "react";

const STATE_COLORS = {
  READY: "blue",
  STARTED: "blue",
  FAILED: "red",
  COMPLETED: "green",
  SUCCESS: "green",
  COMPLETE: "green",
  STOPPED: "amber"
};

const ALL_SOUNDS = ["start", "failed", "success", "complete", "stop", "reset"];

export default function GameCard({ game, live, onCommand, onTestSound }) {
  const isOnline = live?.online;
  const state = (live?.state === "IDLE" ? "READY" : (live?.state || "READY"));
  const attempt = live?.attempt || 0;
  const startTime = live?.startTime;
  const lastHeartbeat = live?.lastHeartbeat;
  const heartbeatCount = live?.heartbeatCount || 0;

  const [elapsed, setElapsed] = useState(0);
  const [showDebug, setShowDebug] = useState(false);
  const [secondsSinceHb, setSecondsSinceHb] = useState(null);

  // Active Game Duration Timer
  useEffect(() => {
    let timer;
    if (state === "STARTED" && startTime) {
      setElapsed(Math.floor((Date.now() - startTime) / 1000));
      timer = setInterval(() => {
        setElapsed(Math.floor((Date.now() - startTime) / 1000));
      }, 1000);
    } else if (state === "COMPLETED" && startTime) {
      setElapsed(Math.floor((Date.now() - startTime) / 1000));
    } else {
      setElapsed(0);
    }
    return () => clearInterval(timer);
  }, [state, startTime]);

  // Live Heartbeat / Signal Relative Age Ticker
  const activePingTime = lastHeartbeat || (isOnline ? live?.lastUpdate : null);

  useEffect(() => {
    if (!activePingTime) {
      setSecondsSinceHb(null);
      return;
    }

    const updateHbAge = () => {
      const diffSec = Math.floor((Date.now() - activePingTime) / 1000);
      setSecondsSinceHb(diffSec >= 0 ? diffSec : 0);
    };

    updateHbAge();
    const interval = setInterval(updateHbAge, 1000);
    return () => clearInterval(interval);
  }, [activePingTime]);

  const formatTimer = (seconds) => {
    const mins = Math.floor(seconds / 60);
    const secs = seconds % 60;
    return `${mins.toString().padStart(2, "0")}:${secs.toString().padStart(2, "0")}`;
  };

  const audioMap = game?.audio || {};

  return (
    <div className={`game-card ${isOnline ? "online" : "offline"} state-${state.toLowerCase()}`}>
      <div className="card-header">
        <div className="title-area">
          <h3>{game.name}</h3>
          <span className="game-id-badge">{game.id}</span>
        </div>
        <div className={`status-badge ${isOnline ? "live" : "offline"}`}>
          <span className="dot"></span>
          {isOnline ? "LIVE" : "OFFLINE"}
        </div>
      </div>

      <div className="card-body">
        <div className="stat-grid">
          <div className="stat-item">
            <span className="stat-label">Current State</span>
            <span className={`state-tag ${STATE_COLORS[state] || "gray"}`}>{state}</span>
          </div>
          <div className="stat-item">
            <span className="stat-label">Attempt</span>
            <span className="stat-value">#{attempt}</span>
          </div>
          {(state === "STARTED" || (state === "COMPLETED" && elapsed > 0)) && (
            <div className="stat-item">
              <span className="stat-label">{state === "COMPLETED" ? "Solve Time" : "Timer"}</span>
              <span className={`stat-value ${state === "COMPLETED" ? "timer-solved" : "timer-active"}`}>
                {formatTimer(elapsed)}
              </span>
            </div>
          )}
        </div>

        {game.description && <p className="description">{game.description}</p>}

        <div className="action-row">
          <button
            className="btn btn-primary"
            disabled={!isOnline}
            onClick={() => onCommand("START")}
            title="Start game puzzle"
          >
            ▶ Start
          </button>
          <button
            className="btn btn-warning"
            disabled={!isOnline}
            onClick={() => onCommand("RESTART")}
            title="Restart puzzle attempt"
          >
            ↻ Restart
          </button>
          <button
            className="btn btn-danger"
            disabled={!isOnline}
            onClick={() => onCommand("STOP")}
            title="Stop/Override puzzle"
          >
            ⏹ Stop
          </button>
          <button
            className="btn btn-secondary"
            disabled={!isOnline}
            onClick={() => onCommand("RESET")}
            title="Reset prop to READY state"
          >
            ⟲ Reset
          </button>
          <button
            className="btn btn-solve"
            disabled={!isOnline}
            onClick={() => {
              if (window.confirm(`⚡ EMERGENCY OVERRIDE: Force solve and unlock '${game.name}'?`)) {
                onCommand("SOLVE");
              }
            }}
            title="Remote Emergency Override / Force Unlock"
          >
            ⚡ Force Solve
          </button>
        </div>

        <div className="sound-tests">
          <span className="sound-label">Audio Test & FX:</span>
          <div className="sound-buttons">
            {ALL_SOUNDS.map((soundKey) => {
              const hasMp3 = Boolean(audioMap[soundKey]);
              return (
                <button
                  key={soundKey}
                  className={`btn-sound ${hasMp3 ? "has-file" : "synth-only"}`}
                  onClick={() => onTestSound(soundKey)}
                  title={hasMp3 ? `Play ${soundKey}.mp3` : `Test ${soundKey} synth sound`}
                >
                  {hasMp3 ? "🔊" : "🎵"} {soundKey}
                </button>
              );
            })}
          </div>
        </div>

        {/* Debug & Heartbeat Toggle Button */}
        <div className="card-footer-controls">
          <button
            className={`btn-debug-toggle ${showDebug ? "active" : ""}`}
            onClick={() => setShowDebug(!showDebug)}
            title="Toggle heartbeat telemetry & hardware diagnostics"
          >
            🩺 {showDebug ? "Hide Diagnostics" : "Debug / Heartbeats"}
          </button>
        </div>

        {/* Collapsible Heartbeat & Diagnostics Drawer */}
        {showDebug && (
          <div className="debug-drawer animate-fade-in">
            <div className="debug-header">
              <span className="debug-title">🔬 Hardware Telemetry</span>
              <span className={`debug-status-pill ${isOnline ? "ok" : "warn"}`}>
                {isOnline ? "🟢 Link Active" : "🔴 Disconnected"}
              </span>
            </div>

            <div className="debug-grid">
              <div className="debug-metric">
                <span className="metric-label">Last Heartbeat</span>
                <span className="metric-value">
                  {secondsSinceHb !== null ? `${secondsSinceHb}s ago` : "None received yet"}
                </span>
              </div>
              <div className="debug-metric">
                <span className="metric-label">Heartbeat Pings</span>
                <span className="metric-value">{heartbeatCount}</span>
              </div>
              <div className="debug-metric">
                <span className="metric-label">Expected Interval</span>
                <span className="metric-value">~3 seconds</span>
              </div>
              <div className="debug-metric">
                <span className="metric-label">MQTT Command Topic</span>
                <code className="metric-code">escaperoom/{game.id}/cmd</code>
              </div>
              <div className="debug-metric">
                <span className="metric-label">MQTT State Topic</span>
                <code className="metric-code">escaperoom/{game.id}/state</code>
              </div>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
