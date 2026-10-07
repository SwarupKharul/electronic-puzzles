// =================================================================================
// ESCAPE ROOM CONTROL SERVER (Production-Grade Orchestrator)
// =================================================================================
// Orchestrates:
//   1. Embedded Aedes MQTT Broker (TCP for ESP32 hardware props)
//   2. MQTT-over-WebSocket Bridge (for real-time browser control dashboard)
//   3. Express REST API & Static Audio / Web App Server
//   4. In-Memory Prop State Registry & Live Diagnostics
//   5. Redundant HTTP Command Dispatch Fallback
//   6. mDNS / Bonjour Advertising (Zero-IP Configuration for ESP32 & Browser)
// =================================================================================

import { createServer as createHttp } from "node:http";
import { createServer as createTcp } from "node:net";
import { readFileSync, existsSync } from "node:fs";
import { networkInterfaces, hostname } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import { WebSocketServer, createWebSocketStream } from "ws";
import Aedes from "aedes";
import { Bonjour } from "bonjour-service";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const ROOT = __dirname;

const HTTP_PORT = Number(process.env.PORT || 3000);
const MQTT_PORT = Number(process.env.MQTT_PORT || 1883);

const SOUNDS = ["start", "failed", "success", "complete", "stop", "reset"];
const EXTS = ["mp3", "wav", "ogg", "m4a"];

// ---------------------------------------------------------------------------------
// Helper: Discover LAN IP addresses
// ---------------------------------------------------------------------------------
function getLanIps() {
  const ips = [];
  const interfaces = networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const net of interfaces[name] || []) {
      if (net.family === "IPv4" && !net.internal) {
        ips.push(net.address);
      }
    }
  }
  return ips.length > 0 ? ips : ["127.0.0.1"];
}

// ---------------------------------------------------------------------------------
// 1. In-Memory Prop State Registry & Hardware Client Mapping
// ---------------------------------------------------------------------------------
let defaultRootTopic = "escaperoom";
try {
  const cfg = JSON.parse(readFileSync(path.join(ROOT, "games.json"), "utf8"));
  if (cfg.rootTopic) defaultRootTopic = cfg.rootTopic;
} catch {}

const propRegistry = new Map(); // gameId -> { online, state, attempt, lastEvent, lastHeartbeat, heartbeatCount, lastSeen }
const clientToGame = new Map(); // clientId -> gameId

function updatePropRegistry(gameId, key, value) {
  if (!propRegistry.has(gameId)) {
    propRegistry.set(gameId, {
      online: false,
      state: "READY",
      attempt: 0,
      lastEvent: null,
      lastHeartbeat: null,
      heartbeatCount: 0,
      lastSeen: Date.now()
    });
  }
  const prop = propRegistry.get(gameId);
  if (key === "heartbeat") {
    prop.lastHeartbeat = value || Date.now();
    prop.heartbeatCount = (prop.heartbeatCount || 0) + 1;
    prop.online = true;
  } else if (key === "online") {
    prop.online = Boolean(value);
  } else {
    prop[key] = value;
  }
  prop.lastSeen = Date.now();
}

function markPropOffline(gameId, reason) {
  const prop = propRegistry.get(gameId);
  if (!prop || !prop.online) return; // already offline

  prop.online = false;
  prop.lastSeen = Date.now();
  console.log(`🔴 [HARDWARE WATCHDOG] Prop '${gameId}' switched to OFFLINE: ${reason}`);

  // Disconnect & prune any stale TCP client associated with this prop to prevent orphaned sockets
  for (const [clientId, mappedId] of clientToGame.entries()) {
    if (mappedId === gameId) {
      clientToGame.delete(clientId);
      const staleClient = aedes.clients && aedes.clients[clientId];
      if (staleClient) {
        try {
          console.log(`🔌 [SOCKET CLEANUP] Terminating stale socket for ${clientId}`);
          staleClient.close();
        } catch {}
      }
    }
  }

  // Broadcast retained offline status to all dashboards & MQTT clients immediately
  aedes.publish(
    {
      cmd: "publish",
      topic: `${defaultRootTopic}/${gameId}/status`,
      payload: Buffer.from("offline"),
      qos: 1,
      retain: true,
      dup: false
    },
    (err) => {
      if (err) console.error(`Failed to publish offline status for ${gameId}:`, err);
    }
  );
}

// Pre-initialize registry for all known games
try {
  const cfg = JSON.parse(readFileSync(path.join(ROOT, "games.json"), "utf8"));
  for (const g of cfg.games || []) {
    propRegistry.set(g.id, {
      online: false,
      state: "READY",
      attempt: 0,
      lastEvent: null,
      lastHeartbeat: null,
      heartbeatCount: 0,
      lastSeen: 0
    });
  }
} catch {}

// ---------------------------------------------------------------------------------
// 2. Embedded Aedes MQTT Broker (High Performance & Resilient)
// ---------------------------------------------------------------------------------
const aedes = Aedes({
  concurrency: 100,
  heartbeatInterval: 60000,
  connectTimeout: 60000
});

let activeMqttPort = MQTT_PORT;
const tcpServer = createTcp(aedes.handle);

// Robust TCP server error handling with auto-fallback for busy ports (e.g. 1883 -> 1884)
tcpServer.on("error", (err) => {
  if (err.code === "EADDRINUSE" && activeMqttPort === MQTT_PORT) {
    activeMqttPort = MQTT_PORT + 1; // Fallback to 1884 if 1883 is used by a daemon
    console.warn(`⚠️ Port ${MQTT_PORT} is in use (e.g. system Mosquitto running). Falling back to MQTT port ${activeMqttPort}...`);
    tcpServer.listen(activeMqttPort, "0.0.0.0");
  } else {
    console.error("❌ MQTT TCP Server Error:", err);
  }
});

tcpServer.listen(activeMqttPort, "0.0.0.0", () => {
  console.log(`📡 Embedded MQTT Broker listening on TCP port ${activeMqttPort}`);
  advertiseMdnsServices();

  // Broadcast initial retained offline status for all games so no stale 'online' lingers on broker start
  for (const gameId of propRegistry.keys()) {
    aedes.publish({
      cmd: "publish",
      topic: `${defaultRootTopic}/${gameId}/status`,
      payload: Buffer.from("offline"),
      qos: 1,
      retain: true,
      dup: false
    });
  }
});

function getGameIdForClient(clientId) {
  if (!clientId) return null;
  if (clientToGame.has(clientId)) return clientToGame.get(clientId);

  // Check known game IDs first (e.g. game1, game2, dolls, etc.)
  for (const knownId of propRegistry.keys()) {
    if (clientId.toLowerCase().includes(knownId.toLowerCase())) {
      return knownId;
    }
  }

  const match = clientId.match(/game[0-9]+/i);
  return match ? match[0].toLowerCase() : null;
}

// Aedes Broker Lifecycle & Event Listeners
aedes.on("client", (client) => {
  console.log(`🟢 [MQTT Client Connected] ID: ${client.id}`);
  const gameId = getGameIdForClient(client.id);
  if (gameId) {
    clientToGame.set(client.id, gameId);
  }
});

aedes.on("clientDisconnect", (client) => {
  console.log(`🔴 [MQTT Client Disconnected] ID: ${client.id}`);
  const gameId = getGameIdForClient(client.id);
  if (gameId) {
    clientToGame.delete(client.id);
    markPropOffline(gameId, `Physical connection closed by ${client.id}`);
  }
});

aedes.on("clientError", (client, err) => {
  if (
    err?.message?.includes("ECONNRESET") ||
    err?.message?.includes("EPIPE") ||
    err?.message?.includes("CLOSING") ||
    err?.message?.includes("CLOSED")
  ) {
    return;
  }
  console.warn(`⚠️ [MQTT Client Error] ID: ${client?.id || "unknown"} -`, err?.message || err);
});

aedes.on("connectionError", (client, err) => {
  console.warn(`⚠️ [MQTT Connection Error] ID: ${client?.id || "unknown"} -`, err?.message || err);
  const gameId = getGameIdForClient(client?.id);
  if (gameId) {
    clientToGame.delete(client.id);
    markPropOffline(gameId, `Socket error on ${client.id}`);
  }
});

aedes.on("keepaliveTimeout", (client) => {
  console.warn(`⏱️ [MQTT Keepalive Timeout] Disconnecting stale client: ${client?.id}`);
  const gameId = getGameIdForClient(client?.id);
  if (gameId) {
    clientToGame.delete(client.id);
    markPropOffline(gameId, `Keepalive timeout on ${client.id}`);
  }
});

// Live message tracking & Registry sync
aedes.on("publish", (packet, client) => {
  if (!packet?.topic || packet.topic.startsWith("$SYS")) return;

  const topic = packet.topic;
  const payloadStr = packet.payload ? packet.payload.toString() : "";

  console.log(`📨 [MQTT] ${topic} => ${payloadStr}`);

  const parts = topic.split("/");
  if (parts.length >= 3) {
    const gameId = parts[1];
    const kind = parts[2];

    if (client?.id && !client.id.startsWith("dash-")) {
      clientToGame.set(client.id, gameId);
    }

    if (kind === "status") {
      updatePropRegistry(gameId, "online", payloadStr === "online");
    } else if (kind === "state") {
      updatePropRegistry(gameId, "online", true);
      let stateVal = "READY";
      let attemptVal = undefined;
      try {
        const parsed = JSON.parse(payloadStr);
        stateVal = (parsed.state || payloadStr).toUpperCase();
        attemptVal = parsed.attempt;
      } catch {
        stateVal = payloadStr.toUpperCase();
      }
      if (stateVal === "IDLE") stateVal = "READY";
      if (stateVal === "SUCCESS" || stateVal === "COMPLETE") stateVal = "COMPLETED";

      updatePropRegistry(gameId, "state", stateVal);
      if (attemptVal !== undefined) updatePropRegistry(gameId, "attempt", attemptVal);
    } else if (kind === "event") {
      updatePropRegistry(gameId, "online", true);
      try {
        const parsed = JSON.parse(payloadStr);
        const eventName = (parsed.event || payloadStr).toUpperCase();
        if (eventName === "HEARTBEAT") {
          updatePropRegistry(gameId, "heartbeat", Date.now());
        } else {
          updatePropRegistry(gameId, "lastEvent", { name: parsed.event || payloadStr, at: Date.now() });
          if (parsed.attempt !== undefined) updatePropRegistry(gameId, "attempt", parsed.attempt);
        }
      } catch {
        if (payloadStr.toUpperCase() === "HEARTBEAT") {
          updatePropRegistry(gameId, "heartbeat", Date.now());
        } else {
          updatePropRegistry(gameId, "lastEvent", { name: payloadStr, at: Date.now() });
        }
      }
    }
  }
});

// ---------------------------------------------------------------------------------
// Real-Time Prop Heartbeat Watchdog
// Sweeps every 1000ms: If a prop stops sending heartbeats/traffic for >10 seconds
// (3 consecutive missed 3s heartbeats), it is immediately marked as OFFLINE,
// stale TCP sockets are pruned, and retained offline status is broadcast to all dashboards.
// ---------------------------------------------------------------------------------
const HEARTBEAT_TIMEOUT_MS = 10000; // 10 seconds

setInterval(() => {
  const now = Date.now();
  for (const [gameId, prop] of propRegistry.entries()) {
    if (prop.online) {
      const lastActivity = Math.max(prop.lastHeartbeat || 0, prop.lastSeen || 0);
      const elapsed = now - lastActivity;
      if (lastActivity > 0 && elapsed > HEARTBEAT_TIMEOUT_MS) {
        markPropOffline(gameId, `No heartbeat for ${(elapsed / 1000).toFixed(1)}s (powered off / link severed)`);
      }
    }
  }
}, 1000);

// ---------------------------------------------------------------------------------
// 3. mDNS / Bonjour Advertising (Zero-IP Configuration)
// ---------------------------------------------------------------------------------
const bonjour = new Bonjour();

function advertiseMdnsServices() {
  const ipList = getLanIps();
  const primaryIp = ipList[0] || "127.0.0.1";
  const osHost = hostname();

  try {
    // 1. Advertise MQTT Service for ESP32 props (discovers both IP and active port)
    bonjour.publish({
      name: "EscapeRoomMQTT",
      type: "mqtt",
      protocol: "tcp",
      port: activeMqttPort,
      host: "escaperoom.local",
      txt: {
        app: "escape-room-control",
        ip: primaryIp,
        host: osHost,
        version: "2.0"
      }
    });

    // 2. Advertise HTTP Web Dashboard for browsers, tablets & phones
    bonjour.publish({
      name: "EscapeRoomWeb",
      type: "http",
      protocol: "tcp",
      port: HTTP_PORT,
      host: "escaperoom.local",
      txt: {
        path: "/",
        ip: primaryIp
      }
    });

    console.log("\n📡 [mDNS Active] Zero-Configuration Discovery Online:");
    console.log(`   🌐 Web Dashboard : http://escaperoom.local:${HTTP_PORT} (or http://${osHost}.local:${HTTP_PORT})`);
    console.log(`   ⚡ MQTT Service  : _mqtt._tcp on port ${activeMqttPort} (Auto-detected by ESP32 props)\n`);
  } catch (err) {
    console.warn("⚠️ mDNS publish warning:", err.message);
  }
}

// ---------------------------------------------------------------------------------
// 4. Express HTTP & REST API Server
// ---------------------------------------------------------------------------------
const app = express();
app.use(express.json());

// CORS & Security Headers
app.use((req, res, next) => {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.header("Access-Control-Allow-Headers", "Content-Type, Authorization");
  if (req.method === "OPTIONS") return res.sendStatus(200);
  next();
});

// Config API endpoint (scans local games.json & audio directory)
app.get("/api/config", (req, res) => {
  try {
    const configPath = path.join(ROOT, "games.json");
    if (!existsSync(configPath)) {
      return res.status(404).json({ error: "games.json not found" });
    }

    const cfg = JSON.parse(readFileSync(configPath, "utf8"));
    const games = (cfg.games || []).map((g) => {
      const folder = g.audioFolder || g.id;
      const audio = {};
      for (const sound of SOUNDS) {
        for (const ext of EXTS) {
          const filePath = path.join(ROOT, "audio", folder, `${sound}.${ext}`);
          if (existsSync(filePath)) {
            audio[sound] = `/audio/${folder}/${sound}.${ext}`;
            break;
          }
        }
      }

      const liveState = propRegistry.get(g.id) || {
        online: false,
        state: "READY",
        attempt: 0,
        lastEvent: null,
        lastHeartbeat: null,
        heartbeatCount: 0
      };

      return {
        id: g.id,
        name: g.name,
        description: g.description || "",
        audio,
        liveState
      };
    });

    res.json({
      rootTopic: cfg.rootTopic || "escaperoom",
      games,
      info: {
        ips: getLanIps(),
        mqttPort: activeMqttPort,
        httpPort: HTTP_PORT,
        hostname: `${hostname()}.local`,
        mdnsHost: "escaperoom.local",
        connectedClients: aedes.connectedClients
      }
    });
  } catch (err) {
    res.status(500).json({ error: `Failed to load games.json: ${err.message}` });
  }
});

// Direct HTTP Command Dispatch (Fail-safe backup if WebSocket is disconnected)
app.post("/api/games/:id/cmd", (req, res) => {
  const gameId = req.params.id;
  const cmd = req.body.cmd;

  if (!cmd) {
    return res.status(400).json({ error: "Missing 'cmd' in request body" });
  }

  const configPath = path.join(ROOT, "games.json");
  let rootTopic = "escaperoom";
  if (existsSync(configPath)) {
    try {
      const cfg = JSON.parse(readFileSync(configPath, "utf8"));
      rootTopic = cfg.rootTopic || rootTopic;
    } catch {}
  }

  const topic = `${rootTopic}/${gameId}/cmd`;
  aedes.publish(
    {
      topic,
      payload: Buffer.from(cmd),
      qos: 1,
      retain: false
    },
    (err) => {
      if (err) {
        console.error(`❌ Failed to dispatch command via HTTP to ${topic}:`, err);
        return res.status(500).json({ error: "Broker failed to publish command" });
      }
      console.log(`⚡ [HTTP API -> MQTT] Dispatched command '${cmd}' to ${topic}`);
      res.json({ status: "ok", gameId, cmd, topic });
    }
  );
});

// System Health & Diagnostics Endpoint
app.get("/api/status", (req, res) => {
  res.json({
    status: "online",
    uptimeSeconds: Math.floor(process.uptime()),
    mqttPort: activeMqttPort,
    httpPort: HTTP_PORT,
    hostname: `${hostname()}.local`,
    mdnsHost: "escaperoom.local",
    connectedMqttClients: aedes.connectedClients,
    props: Object.fromEntries(propRegistry),
    memoryUsageMB: Math.round(process.memoryUsage().heapUsed / 1024 / 1024)
  });
});

// Serve local audio files
app.use("/audio", express.static(path.join(ROOT, "audio")));

// Serve built React static frontend
app.use(express.static(path.join(ROOT, "dist")));

// Fallback for SPA routing
app.use((req, res, next) => {
  const distIndex = path.join(ROOT, "dist", "index.html");
  if (existsSync(distIndex)) {
    res.sendFile(distIndex);
  } else {
    res.status(200).send(`
      <!DOCTYPE html>
      <html>
        <head><title>Escape Room Control Server</title></head>
        <body style="font-family:sans-serif; background:#0f172a; color:#f8fafc; padding:40px; text-align:center;">
          <h2>🔐 Escape Room Control Server Running</h2>
          <p>The backend and MQTT broker are active.</p>
          <p>Dashboard URL: <a href="http://escaperoom.local:3000" style="color:#38bdf8;">http://escaperoom.local:3000</a></p>
        </body>
      </html>
    `);
  }
});

// ---------------------------------------------------------------------------------
// 5. HTTP + WebSocket Server for Browser UI
// ---------------------------------------------------------------------------------
const httpServer = createHttp(app);
const wss = new WebSocketServer({
  server: httpServer,
  path: "/mqtt",
  handleProtocols: (protocols) => {
    // Negotiate 'mqtt' or 'mqttv3.1' subprotocols requested by browser mqtt.js clients
    if (protocols.has("mqtt")) return "mqtt";
    if (protocols.has("mqttv3.1")) return "mqttv3.1";
    return Array.from(protocols)[0] || false;
  }
});

wss.on("connection", (ws, req) => {
  const stream = createWebSocketStream(ws);

  stream.on("error", (err) => {
    if (
      err?.message?.includes("CLOSING") ||
      err?.message?.includes("CLOSED") ||
      err?.message?.includes("not open") ||
      err?.message?.includes("ECONNRESET")
    ) {
      return;
    }
    console.warn("⚠️ WebSocket stream error:", err.message || err);
  });

  ws.on("error", () => {});

  aedes.handle(stream, req);
});

httpServer.listen(HTTP_PORT, "0.0.0.0", () => {
  const ipList = getLanIps();
  const primaryIp = ipList[0] || "localhost";
  const osHost = hostname();

  console.log("\n=======================================================");
  console.log(" 🔐 ESCAPE ROOM CONTROL CENTER (PRODUCTION READY)");
  console.log("-------------------------------------------------------");
  console.log(`  🌐 Dashboard URL      : http://${primaryIp}:${HTTP_PORT}`);
  console.log(`  📡 mDNS Host URL      : http://escaperoom.local:${HTTP_PORT} (or http://${osHost}.local:${HTTP_PORT})`);
  console.log(`  ⚡ ESP32 Broker Host  : escaperoom.local / ${primaryIp}`);
  console.log(`  🔌 ESP32 Broker Port  : ${activeMqttPort} (TCP)`);
  console.log(`  💬 WebSocket Endpoint : ws://${primaryIp}:${HTTP_PORT}/mqtt`);
  console.log(`  🔍 Healthcheck API    : http://${primaryIp}:${HTTP_PORT}/api/status`);
  console.log("=======================================================\n");
});

// ---------------------------------------------------------------------------------
// 6. Graceful Process Teardown
// ---------------------------------------------------------------------------------
function gracefulShutdown(signal) {
  console.log(`\n🛑 Received ${signal}. Shutting down Escape Room Control Server gracefully...`);

  try {
    bonjour.destroy();
  } catch {}

  wss.close(() => {
    httpServer.close(() => {
      aedes.close(() => {
        tcpServer.close(() => {
          console.log("✅ All servers, brokers, and mDNS records closed cleanly. Goodbye!");
          process.exit(0);
        });
      });
    });
  });

  setTimeout(() => {
    console.warn("⚠️ Forcefully terminating process.");
    process.exit(0);
  }, 3000).unref();
}

process.on("SIGINT", () => gracefulShutdown("SIGINT"));
process.on("SIGTERM", () => gracefulShutdown("SIGTERM"));
process.on("uncaughtException", (err) => {
  console.error("💥 Uncaught Exception:", err);
});
process.on("unhandledRejection", (reason) => {
  console.warn("⚠️ Unhandled Promise Rejection:", reason);
});
