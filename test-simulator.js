// =================================================================================
// ESCAPE ROOM UNIVERSAL PROP SIMULATOR (Interactive & Automated CLI)
// =================================================================================
// Features:
//   1. Dynamic Game Discovery: Automatically loads all games from games.json
//   2. Interactive Terminal CLI: Send any event or state to any prop on demand
//   3. CLI Direct Mode: e.g. `node test-simulator.js game2 SUCCESS`
//   4. Automatic Simulation Mode: Auto-plays game cycles for stress-testing
//   5. Live Command Listener: Responds to Dashboard GM commands (START, STOP, RESET, SOLVE)
// =================================================================================

import mqtt from "mqtt";
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import readline from "node:readline";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const ROOT_DIR = __dirname;

// Load games.json
const CONFIG_PATH = path.join(ROOT_DIR, "games.json");
let config = {
  rootTopic: "escaperoom",
  games: [
    { id: "game1", name: "Knock Puzzle Prop" },
    { id: "game2", name: "Dolls Puzzle" },
    { id: "game3", name: "Coins Puzzle Prop" }
  ]
};

if (existsSync(CONFIG_PATH)) {
  try {
    config = JSON.parse(readFileSync(CONFIG_PATH, "utf8"));
  } catch (err) {
    console.warn("⚠️ Could not parse games.json, using fallback config.");
  }
}

const BROKER_PORT = process.env.MQTT_PORT || 1884;
const BROKER_URL = process.env.MQTT_URL || `mqtt://localhost:${BROKER_PORT}`;
const ROOT = config.rootTopic || "escaperoom";

console.log("\n=================================================================");
console.log(" 🎮 ESCAPE ROOM UNIVERSAL PROP SIMULATOR");
console.log(` 📡 MQTT Broker: ${BROKER_URL} | Root Topic: ${ROOT}`);
console.log("=================================================================");

// Prop Store: gameId -> { client, game, state, attempt, isOnline, autoTimer }
const props = new Map();
let autoSimActive = false;

// ---------------------------------------------------------------------------------
// Prop Simulator Factory
// ---------------------------------------------------------------------------------
function createProp(game) {
  const gameId = game.id;
  const name = game.name || gameId;

  const client = mqtt.connect(BROKER_URL, {
    clientId: `sim-${gameId}-${Math.random().toString(16).slice(2, 6)}`,
    will: {
      topic: `${ROOT}/${gameId}/status`,
      payload: "offline",
      retain: true,
      qos: 1
    },
    keepalive: 60,
    reconnectPeriod: 3000
  });

  const propObj = {
    gameId,
    name,
    client,
    state: "READY",
    attempt: 0,
    isOnline: false,
    autoTimer: null
  };

  client.on("connect", () => {
    propObj.isOnline = true;
    console.log(`🟢 [${gameId}] '${name}' Simulator CONNECTED & ONLINE`);
    client.publish(`${ROOT}/${gameId}/status`, "online", { retain: true, qos: 1 });
    publishState(gameId, propObj.state);
    client.subscribe(`${ROOT}/${gameId}/cmd`);

    if (propObj.hbInterval) clearInterval(propObj.hbInterval);
    propObj.hbInterval = setInterval(() => {
      if (propObj.isOnline) {
        client.publish(`${ROOT}/${gameId}/event`, JSON.stringify({ event: "HEARTBEAT", attempt: propObj.attempt }));
      }
    }, 3000);
  });

  client.on("close", () => {
    propObj.isOnline = false;
    if (propObj.hbInterval) clearInterval(propObj.hbInterval);
  });

  client.on("error", (err) => {
    console.warn(`⚠️ [${gameId}] MQTT Error:`, err.message);
  });

  client.on("message", (topic, payload) => {
    const cmd = payload.toString().trim().toUpperCase();
    console.log(`\n📩 [${gameId}] Received Command from Dashboard: "${cmd}"`);

    if (cmd === "START" || cmd === "RESTART") {
      propObj.attempt++;
      publishState(gameId, "STARTED");
      publishEvent(gameId, "STARTED");
    } else if (cmd === "STOP") {
      publishState(gameId, "STOPPED");
      publishEvent(gameId, "STOPPED");
    } else if (cmd === "RESET") {
      propObj.attempt = 0;
      publishState(gameId, "READY");
      publishEvent(gameId, "RESET");
    } else if (cmd === "SOLVE" || cmd === "OVERRIDE") {
      publishState(gameId, "COMPLETED");
      publishEvent(gameId, "COMPLETED");
    }
    showPrompt();
  });

  props.set(gameId, propObj);
  return propObj;
}

// ---------------------------------------------------------------------------------
// Event & State Publishers
// ---------------------------------------------------------------------------------
function publishState(gameId, state) {
  const prop = props.get(gameId);
  if (!prop || !prop.client) return;

  prop.state = state;
  const payload = JSON.stringify({ state, attempt: prop.attempt });
  prop.client.publish(`${ROOT}/${gameId}/state`, payload, { retain: true, qos: 1 });
  console.log(`📊 [${gameId}] State -> ${state} (Attempt #${prop.attempt})`);
}

function publishEvent(gameId, eventName) {
  const prop = props.get(gameId);
  if (!prop || !prop.client) return;

  const payload = JSON.stringify({ event: eventName, attempt: prop.attempt });
  prop.client.publish(`${ROOT}/${gameId}/event`, payload, { retain: false, qos: 1 });
  console.log(`⚡ [${gameId}] Event -> ${eventName}`);
}

function setPropStatus(gameId, online) {
  const prop = props.get(gameId);
  if (!prop || !prop.client) return;

  prop.isOnline = online;
  const statusStr = online ? "online" : "offline";
  prop.client.publish(`${ROOT}/${gameId}/status`, statusStr, { retain: true, qos: 1 });
  console.log(`🔌 [${gameId}] Status -> ${statusStr.toUpperCase()}`);
}

// ---------------------------------------------------------------------------------
// Auto Simulation Cycle
// ---------------------------------------------------------------------------------
function toggleAutoSimulation() {
  autoSimActive = !autoSimActive;
  if (autoSimActive) {
    console.log("▶️ Auto-simulation STARTED (props will cycle automatically)");
    props.forEach((prop) => {
      startPropAutoCycle(prop);
    });
  } else {
    console.log("⏸️ Auto-simulation STOPPED");
    props.forEach((prop) => {
      if (prop.autoTimer) clearInterval(prop.autoTimer);
      prop.autoTimer = null;
    });
  }
}

function startPropAutoCycle(prop) {
  if (prop.autoTimer) clearInterval(prop.autoTimer);

  const interval = 5000 + Math.floor(Math.random() * 4000);
  prop.autoTimer = setInterval(() => {
    if (!autoSimActive) return;

    if (prop.state === "READY") {
      prop.attempt++;
      publishState(prop.gameId, "STARTED");
      publishEvent(prop.gameId, "STARTED");
    } else if (prop.state === "STARTED") {
      if (prop.attempt % 2 === 1) {
        publishState(prop.gameId, "FAILED");
        publishEvent(prop.gameId, "FAILED");
      } else {
        publishState(prop.gameId, "COMPLETED");
        publishEvent(prop.gameId, "COMPLETED");
      }
    } else if (prop.state === "FAILED") {
      prop.attempt++;
      publishState(prop.gameId, "STARTED");
      publishEvent(prop.gameId, "STARTED");
    } else if (prop.state === "COMPLETED" || prop.state === "SUCCESS" || prop.state === "COMPLETE" || prop.state === "STOPPED") {
      publishState(prop.gameId, "READY");
      publishEvent(prop.gameId, "RESET");
    }
  }, interval);
}

// ---------------------------------------------------------------------------------
// Initialize All Configured Props
// ---------------------------------------------------------------------------------
(config.games || []).forEach((game) => {
  createProp(game);
});

// ---------------------------------------------------------------------------------
// CLI Argument Handling (One-Shot Execution)
// e.g., `node test-simulator.js game1 SUCCESS` or `node test-simulator.js all RESET`
// ---------------------------------------------------------------------------------
const cliArgs = process.argv.slice(2);
if (cliArgs.length >= 2) {
  const targetGame = cliArgs[0].toLowerCase();
  const targetAction = cliArgs[1].toUpperCase();

  setTimeout(() => {
    const targets = targetGame === "all" ? Array.from(props.keys()) : [targetGame];
    targets.forEach((gId) => {
      if (!props.has(gId)) {
        console.error(`❌ Unknown game ID: '${gId}'`);
        return;
      }
      if (targetAction === "ONLINE" || targetAction === "OFFLINE") {
        setPropStatus(gId, targetAction === "ONLINE");
      } else if (targetAction === "START" || targetAction === "STARTED") {
        const p = props.get(gId);
        p.attempt++;
        publishState(gId, "STARTED");
        publishEvent(gId, "STARTED");
      } else {
        publishState(gId, targetAction);
        publishEvent(gId, targetAction);
      }
    });
  }, 1000);
} else if (cliArgs.includes("--auto")) {
  setTimeout(() => toggleAutoSimulation(), 1000);
}

// ---------------------------------------------------------------------------------
// Interactive Terminal Prompt (Readline)
// ---------------------------------------------------------------------------------
const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});

function printHelp() {
  console.log("\n-----------------------------------------------------------------");
  console.log("📖 SIMULATOR COMMANDS & SYNTAX:");
  console.log("-----------------------------------------------------------------");
  console.log("  <gameId> <event>       -> Send event & state (e.g. 'game1 SUCCESS')");
  console.log("  <gameId> online/offline-> Toggle prop connection status");
  console.log("  all <event>            -> Send event to all props (e.g. 'all RESET')");
  console.log("  auto                   -> Toggle automatic simulation loop on/off");
  console.log("  list                   -> Display all props and current status");
  console.log("  help / ?               -> Show this command help");
  console.log("  exit / quit            -> Quit simulator");
  console.log("-----------------------------------------------------------------");
  console.log("Available Prop IDs: " + Array.from(props.keys()).join(", "));
  console.log("Available Events  : READY, STARTED, COMPLETED, FAILED, RESET, STOPPED, HEARTBEAT\n");
}

function listProps() {
  console.log("\n📋 LIVE PROP STATUS:");
  props.forEach((prop, id) => {
    console.log(`  • [${id}] ${prop.name.padEnd(20)} | Status: ${prop.isOnline ? "🟢 ONLINE" : "🔴 OFFLINE"} | State: ${prop.state} (Attempt #${prop.attempt})`);
  });
  console.log();
}

function showPrompt() {
  rl.prompt();
}

rl.setPrompt("🎮 simulator > ");
setTimeout(() => {
  printHelp();
  listProps();
  showPrompt();
}, 800);

rl.on("line", (line) => {
  const input = line.trim();
  if (!input) {
    showPrompt();
    return;
  }

  const parts = input.split(/\s+/);
  const cmd = parts[0].toLowerCase();

  if (cmd === "exit" || cmd === "quit") {
    console.log("👋 Exiting simulator...");
    process.exit(0);
  } else if (cmd === "help" || cmd === "?") {
    printHelp();
  } else if (cmd === "list") {
    listProps();
  } else if (cmd === "auto") {
    toggleAutoSimulation();
  } else if (parts.length >= 2) {
    const targetGame = parts[0];
    const action = parts[1].toUpperCase();

    const targets = targetGame === "all" ? Array.from(props.keys()) : [targetGame];

    targets.forEach((gId) => {
      const prop = props.get(gId);
      if (!prop) {
        console.log(`❌ Unknown prop ID: '${gId}'. Type 'list' to see available props.`);
        return;
      }

      if (action === "ONLINE") {
        setPropStatus(gId, true);
      } else if (action === "OFFLINE") {
        setPropStatus(gId, false);
      } else if (action === "START" || action === "STARTED") {
        prop.attempt++;
        publishState(gId, "STARTED");
        publishEvent(gId, "STARTED");
      } else if (action === "STOP" || action === "STOPPED") {
        publishState(gId, "STOPPED");
        publishEvent(gId, "STOPPED");
      } else if (action === "RESET" || action === "READY" || action === "IDLE") {
        prop.attempt = 0;
        publishState(gId, "READY");
        publishEvent(gId, "RESET");
      } else if (action === "COMPLETED" || action === "SUCCESS" || action === "SOLVE" || action === "COMPLETE") {
        publishState(gId, "COMPLETED");
        publishEvent(gId, "COMPLETED");
      } else if (action === "FAILED" || action === "FAIL") {
        publishState(gId, "FAILED");
        publishEvent(gId, "FAILED");
      } else {
        // Custom event
        publishEvent(gId, action);
      }
    });
  } else {
    console.log(`⚠️ Unrecognized command: '${input}'. Type 'help' for examples.`);
  }

  showPrompt();
});
