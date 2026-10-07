import React, { useState } from "react";

export default function EventLog({ log, gamesById }) {
  const [filterGame, setFilterGame] = useState("all");

  const filteredLog = log.filter(
    (item) => filterGame === "all" || item.gameId === filterGame
  );

  const formatTime = (ts) => {
    const d = new Date(ts);
    return d.toLocaleTimeString() + "." + d.getMilliseconds().toString().padStart(3, "0");
  };

  return (
    <div className="event-log-container">
      <div className="log-header">
        <h4>📋 Real-time Room Event Log</h4>
        <div className="log-controls">
          <label htmlFor="log-filter">Filter: </label>
          <select
            id="log-filter"
            value={filterGame}
            onChange={(e) => setFilterGame(e.target.value)}
          >
            <option value="all">All Props</option>
            {Object.values(gamesById).map((g) => (
              <option key={g.id} value={g.id}>
                {g.name} ({g.id})
              </option>
            ))}
          </select>
        </div>
      </div>

      <div className="log-list">
        {filteredLog.length === 0 ? (
          <div className="log-empty">No events logged yet. Operational events will stream here instantly.</div>
        ) : (
          filteredLog.map((entry) => {
            const gameName = gamesById[entry.gameId]?.name || entry.gameId;
            return (
              <div key={entry.id} className={`log-entry ${entry.kind || "info"}`}>
                <span className="log-time">{formatTime(entry.at)}</span>
                <span className="log-prop">[{gameName}]</span>
                <span className="log-text">{entry.text}</span>
              </div>
            );
          })
        )}
      </div>
    </div>
  );
}
