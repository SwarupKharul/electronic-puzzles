import { useEffect, useReducer, useRef, useState, useCallback } from "react";
import mqtt from "mqtt";

// ---------------------------------------------------------------------------------
// Initial state template for each game prop
// ---------------------------------------------------------------------------------
const initialGameState = {
  online: false,
  state: "READY",
  attempt: 0,
  lastEvent: null,
  lastHeartbeat: null,
  heartbeatCount: 0,
  lastUpdate: Date.now(),
  startTime: null
};

// ---------------------------------------------------------------------------------
// Pure state reducer — every MQTT message dispatches here
// ---------------------------------------------------------------------------------
function reducer(games, action) {
  const g = games[action.gameId] || { ...initialGameState };
  const now = Date.now();

  switch (action.type) {
    case "status":
      return {
        ...games,
        [action.gameId]: {
          ...g,
          online: action.online,
          lastUpdate: now
        }
      };

    case "heartbeat":
      return {
        ...games,
        [action.gameId]: {
          ...g,
          online: true,
          lastHeartbeat: action.at,
          heartbeatCount: (g.heartbeatCount || 0) + 1,
          lastUpdate: action.at
        }
      };

    case "state": {
      let stateName = (action.state || "").toUpperCase();
      if (stateName === "IDLE") stateName = "READY";
      if (stateName === "SUCCESS" || stateName === "COMPLETE") stateName = "COMPLETED";

      return {
        ...games,
        [action.gameId]: {
          ...g,
          online: true, // Any state message proves the prop is alive
          state: stateName,
          attempt: action.attempt ?? g.attempt,
          startTime:
            stateName === "STARTED"
              ? g.startTime || now
              : stateName === "READY"
              ? null
              : g.startTime,
          lastUpdate: now
        }
      };
    }

    case "event":
      return {
        ...games,
        [action.gameId]: {
          ...g,
          online: true, // Any event proves the prop is alive
          lastEvent: { name: action.name, at: now, attempt: action.attempt },
          attempt: action.attempt ?? g.attempt,
          lastUpdate: now
        }
      };

    default:
      return games;
  }
}

// ---------------------------------------------------------------------------------
// Main Hook: useEscapeRoom
// ---------------------------------------------------------------------------------
export function useEscapeRoom(config, onEvent) {
  const [connected, setConnected] = useState(false);
  const [games, dispatch] = useReducer(reducer, null, () =>
    Object.fromEntries(
      (config?.games || []).map((g) => [
        g.id,
        {
          ...initialGameState,
          online: g.liveState?.online ?? false,
          state: (g.liveState?.state === "IDLE" ? "READY" : (g.liveState?.state || "READY")),
          attempt: g.liveState?.attempt ?? 0,
          lastEvent: g.liveState?.lastEvent ?? null,
          lastHeartbeat: g.liveState?.lastHeartbeat ?? null,
          heartbeatCount: g.liveState?.heartbeatCount ?? 0
        }
      ])
    )
  );
  const [log, setLog] = useState([]);
  const clientRef = useRef(null);
  const onEventRef = useRef(onEvent);
  onEventRef.current = onEvent;

  // Stable ref to latest games state for watchdog (avoids re-render loops)
  const gamesRef = useRef(games);
  gamesRef.current = games;

  const addLog = useCallback((gameId, text, kind = "info") => {
    setLog((l) =>
      [
        {
          id: Math.random().toString(36).substring(2, 9),
          at: Date.now(),
          gameId,
          text,
          kind
        },
        ...l
      ].slice(0, 150)
    );
  }, []);

  // Stable client ID across re-renders
  const clientIdRef = useRef(
    "dash-" + Math.random().toString(16).slice(2, 10)
  );

  // Extract rootTopic as a stable primitive for useEffect deps
  const rootTopic = config?.rootTopic;

  // ---------------------------------------------------------------------------
  // Client-Side Fail-Safe Watchdog (runs independently of games state)
  // Uses a ref to access latest games without causing effect re-runs.
  // ---------------------------------------------------------------------------
  useEffect(() => {
    const OFFLINE_TIMEOUT_MS = 10000; // 10s client fail-safe
    const timer = setInterval(() => {
      const now = Date.now();
      const currentGames = gamesRef.current;
      if (!currentGames) return;

      for (const [gameId, g] of Object.entries(currentGames)) {
        if (g.online) {
          const lastActivity = Math.max(
            g.lastHeartbeat || 0,
            g.lastUpdate || 0
          );
          if (lastActivity > 0 && now - lastActivity > OFFLINE_TIMEOUT_MS) {
            dispatch({ type: "status", gameId, online: false });
            addLog(
              gameId,
              "Prop went OFFLINE (No signal for >10s)",
              "status-offline"
            );
          }
        }
      }
    }, 1000);
    return () => clearInterval(timer);
  }, [addLog]); // addLog is stable (useCallback with [])

  // ---------------------------------------------------------------------------
  // MQTT WebSocket Connection & Subscriptions
  // ---------------------------------------------------------------------------
  useEffect(() => {
    if (!rootTopic) return;

    const proto = location.protocol === "https:" ? "wss" : "ws";
    const host = location.hostname || "localhost";
    const port =
      location.port || (location.protocol === "https:" ? "443" : "80");
    const wsUrl = `${proto}://${host}:${port}/mqtt`;

    console.log(
      `🔌 [Dashboard] Connecting to MQTT WebSocket: ${wsUrl} (Client ID: ${clientIdRef.current})`
    );

    const client = mqtt.connect(wsUrl, {
      reconnectPeriod: 2000,
      clientId: clientIdRef.current,
      keepalive: 60,
      clean: true
    });

    clientRef.current = client;

    client.on("connect", () => {
      setConnected(true);
      console.log("✅ [Dashboard] Connected to MQTT WebSocket!");

      // CRITICAL: Use rootTopic (not undefined 'root') for subscriptions
      const topics = [
        `${rootTopic}/+/status`,
        `${rootTopic}/+/state`,
        `${rootTopic}/+/event`
      ];
      console.log(`📡 [Dashboard] Subscribing to: ${topics.join(", ")}`);
      client.subscribe(topics, { qos: 1 }, (err, granted) => {
        if (err) {
          console.error("❌ MQTT subscription error:", err);
        } else {
          console.log(
            "✅ [Dashboard] Subscribed:",
            granted.map((g) => `${g.topic} (QoS ${g.qos})`).join(", ")
          );
        }
      });
    });

    client.on("reconnect", () => {
      console.log("🔄 [Dashboard] Reconnecting to MQTT WebSocket...");
    });

    client.on("close", () => setConnected(false));
    client.on("offline", () => setConnected(false));
    client.on("error", (err) =>
      console.warn("⚠️ MQTT client error:", err.message || err)
    );

    client.on("message", (topic, payload, packet) => {
      const now = Date.now();
      const parts = topic.split("/");
      if (parts.length < 3) return;
      const gameId = parts[1];
      const kind = parts[2];
      const text = payload.toString();

      if (kind === "status") {
        const online = text === "online";
        dispatch({ type: "status", gameId, online });
        addLog(
          gameId,
          online ? "Prop came ONLINE" : "Prop went OFFLINE",
          online ? "status-online" : "status-offline"
        );
      } else if (kind === "state") {
        try {
          const s = JSON.parse(text);
          dispatch({
            type: "state",
            gameId,
            state: s.state,
            attempt: s.attempt
          });
        } catch {
          dispatch({ type: "state", gameId, state: text });
        }
      } else if (kind === "event") {
        // Skip retained events to avoid duplicate audio on page refresh
        if (packet.retain) return;

        let eventName = "";
        let attemptVal = undefined;
        try {
          const e = JSON.parse(text);
          eventName = (e.event || "").toUpperCase();
          attemptVal = e.attempt;
        } catch {
          eventName = text.toUpperCase();
        }

        if (eventName === "HEARTBEAT") {
          // Track heartbeat telemetry silently (no log, no audio)
          dispatch({ type: "heartbeat", gameId, at: now });
          return;
        }

        if (eventName === "SUCCESS" || eventName === "COMPLETE") {
          eventName = "COMPLETED";
        }

        dispatch({
          type: "event",
          gameId,
          name: eventName,
          attempt: attemptVal
        });
        addLog(
          gameId,
          `Event: ${eventName} (Attempt #${attemptVal ?? 1})`,
          `event-${eventName.toLowerCase()}`
        );
        onEventRef.current?.(gameId, eventName);
      }
    });

    return () => {
      try {
        client.end(true);
      } catch {}
    };
  }, [rootTopic, addLog]);

  // ---------------------------------------------------------------------------
  // Command Dispatch: WebSocket MQTT primary, HTTP REST fallback
  // ---------------------------------------------------------------------------
  const sendCommand = useCallback(
    async (gameId, cmd) => {
      const client = clientRef.current;

      // 1. Primary path: WebSocket MQTT
      if (client && client.connected) {
        client.publish(`${config.rootTopic}/${gameId}/cmd`, cmd, { qos: 1 });
        addLog(gameId, `Sent Command: ${cmd} (via MQTT)`, "command");
        return true;
      }

      // 2. Fail-Safe fallback: Direct HTTP POST
      try {
        console.warn(
          `⚠️ WebSocket offline. Falling back to HTTP REST for '${cmd}' on ${gameId}...`
        );
        const res = await fetch(`/api/games/${gameId}/cmd`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ cmd })
        });
        if (res.ok) {
          addLog(gameId, `Sent Command: ${cmd} (via HTTP Fallback)`, "command");
          return true;
        }
      } catch (err) {
        console.error("❌ Failed to send command via HTTP fallback:", err);
      }

      addLog(gameId, `Failed to send command: ${cmd}`, "error");
      return false;
    },
    [config, addLog]
  );

  return { connected, games, log, sendCommand };
}
