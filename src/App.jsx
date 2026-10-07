import React, { useEffect, useMemo, useRef, useState } from "react";
import { useEscapeRoom } from "./useEscapeRoom.js";
import { AudioPlayer, EVENT_SOUND } from "./audio.js";
import GameCard from "./components/GameCard.jsx";
import EventLog from "./components/EventLog.jsx";
import ComputerLinks from "./components/ComputerLinks.jsx";

export default function App() {
  const [config, setConfig] = useState(null);
  const [error, setError] = useState(null);

  useEffect(() => {
    fetch("/api/config")
      .then((r) => r.json())
      .then((c) => (c.error ? setError(c.error) : setConfig(c)))
      .catch(() => setError("Cannot reach the control server. Is 'node index.js' running?"));
  }, []);

  if (error) return <div className="fatal-screen">🚨 {error}</div>;
  if (!config) return <div className="fatal-screen loading">⏳ Loading Escape Room Controller...</div>;
  return <Dashboard config={config} />;
}

function Dashboard({ config }) {
  const params = useMemo(() => new URLSearchParams(window.location.search), []);
  const filter = params.get("game")?.split(",").filter(Boolean) || null;
  const audioWanted = params.get("audio") !== "off";

  const visible = filter
    ? config.games.filter((g) => filter.includes(g.id))
    : config.games;
  const single = visible.length === 1;

  const playerRef = useRef(null);
  if (!playerRef.current) {
    playerRef.current = new AudioPlayer();
  }
  const player = playerRef.current;

  const [audioReady, setAudioReady] = useState(false);
  const [muted, setMuted] = useState(false);
  const [volume, setVolume] = useState(1.0);

  const gamesById = useMemo(
    () => Object.fromEntries(config.games.map((g) => [g.id, g])),
    [config]
  );

  const stateRef = useRef({});
  stateRef.current = { audioReady, muted, visible };

  // Preload all audio assets once config is available
  useEffect(() => {
    if (config?.games) {
      const allUrls = config.games.flatMap((g) => Object.values(g.audio || {}));
      allUrls.filter(Boolean).forEach((url) => {
        player.preload(url).catch(() => {});
      });
    }
  }, [config, player]);

  const onEvent = (gameId, eventName) => {
    const s = stateRef.current;
    if (!audioWanted || s.muted) return;
    if (!s.visible.some((g) => g.id === gameId)) return;

    const soundKey = EVENT_SOUND[eventName] || eventName.toLowerCase();
    let url = gamesById[gameId]?.audio?.[soundKey];
    if (!url && (eventName === "SUCCESS" || eventName === "COMPLETE" || eventName === "COMPLETED")) {
      url = gamesById[gameId]?.audio?.["complete"] || gamesById[gameId]?.audio?.["success"];
    }
    player.play(gameId, url, eventName);
  };

  const { connected, games, log, sendCommand } = useEscapeRoom(config, onEvent);

  const enableAudio = async () => {
    const urls = visible.flatMap((g) => Object.values(g.audio || {}));
    const success = await player.unlock(urls);
    if (success) {
      setAudioReady(true);
      // Play a pleasant chime immediately so the user knows sound is working!
      await player.playSynthSound("SUCCESS");
    }
  };

  const handleMuteToggle = () => {
    const nextMuted = !muted;
    setMuted(nextMuted);
    player.setMuted(nextMuted);
    if (!nextMuted) {
      player.playSynthSound("CLICK");
    }
  };

  const handleVolumeChange = (e) => {
    const val = parseFloat(e.target.value);
    setVolume(val);
    player.setVolume(val);
  };

  const command = (id, cmd) => {
    if (!sendCommand(id, cmd)) {
      alert("Not connected to the MQTT broker server.");
    }
  };

  const bulk = (cmd) => {
    if (!window.confirm(`Are you sure you want to send ${cmd} to ALL props?`)) return;
    visible.forEach((g) => sendCommand(g.id, cmd));
  };

  const testSound = async (gameId, soundKey) => {
    const url = gamesById[gameId]?.audio?.[soundKey];
    const eventName = Object.keys(EVENT_SOUND).find((k) => EVENT_SOUND[k] === soundKey) || soundKey.toUpperCase();
    console.log(`🔊 [Audio] Testing sound: [${gameId}] ${soundKey} -> file: ${url || 'SYNTH FX'}`);
    await player.unlock(url ? [url] : []);
    setAudioReady(true);
    await player.play(gameId, url, eventName);
  };

  const onlineCount = visible.filter((g) => games[g.id]?.online).length;

  return (
    <div className="app-container" onClick={!audioReady ? enableAudio : undefined}>
      <header className="main-header">
        <div className="header-brand">
          <h1>🔐 {single ? visible[0].name : "Escape Room Control Center"}</h1>
          <span className="online-pill">
            <span className="dot"></span>
            {onlineCount} of {visible.length} Props Online
          </span>
        </div>

        <div className="header-actions">
          <span className={`connection-badge ${connected ? "connected" : "disconnected"}`}>
            {connected ? "⚡ Broker Connected" : "❌ Server Disconnected"}
          </span>

          {audioWanted && (
            <div className="audio-controls">
              {!audioReady ? (
                <button className="btn btn-audio-enable pulse" onClick={enableAudio}>
                  🔊 Enable Audio
                </button>
              ) : (
                <div className="audio-active-bar">
                  <button
                    className={`btn ${muted ? "btn-warning" : "btn-success"}`}
                    onClick={handleMuteToggle}
                  >
                    {muted ? "🔇 Muted" : "🔊 Audio On"}
                  </button>
                  <input
                    type="range"
                    min="0"
                    max="1"
                    step="0.05"
                    value={volume}
                    onChange={handleVolumeChange}
                    title={`Volume: ${Math.round(volume * 100)}%`}
                    className="volume-slider"
                  />
                </div>
              )}
            </div>
          )}

          {!single && (
            <div className="bulk-actions">
              <button className="btn btn-danger" onClick={() => bulk("STOP")}>
                ⏹ Stop All
              </button>
              <button className="btn btn-secondary" onClick={() => bulk("RESET")}>
                ⟲ Reset All
              </button>
            </div>
          )}
        </div>
      </header>

      {!connected && (
        <div className="alert-banner error">
          ⚠️ Connection to control server lost. Attempting auto-reconnect... (Props continue operating).
        </div>
      )}

      {audioWanted && !audioReady && (
        <div className="alert-banner warning clickable" onClick={enableAudio}>
          🔊 Sound is currently locked by browser autoplay rules. <strong>Click anywhere or press Enable Audio</strong>.
        </div>
      )}

      <main className={`games-grid ${single ? "single-prop" : ""}`}>
        {visible.map((g) => (
          <GameCard
            key={g.id}
            game={g}
            live={games[g.id]}
            onCommand={(cmd) => command(g.id, cmd)}
            onTestSound={(s) => testSound(g.id, s)}
          />
        ))}
      </main>

      {!single && (
        <footer className="lower-dashboard">
          <EventLog log={log} gamesById={gamesById} />
          <ComputerLinks config={config} />
        </footer>
      )}
    </div>
  );
}
