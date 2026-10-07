import React from "react";

export default function ComputerLinks({ config }) {
  const ips = config?.info?.ips || ["localhost"];
  const port = config?.info?.httpPort || 3000;
  const mqttPort = config?.info?.mqttPort || 1883;

  return (
    <div className="system-links-container">
      <h4>🌐 Network & Device Links</h4>
      <p className="system-desc">
        Use these URLs to connect tablets, secondary monitors, or configure ESP32 prop firmware.
      </p>

      <div className="link-sections">
        <div className="link-card">
          <h5>📱 Game Master Tablet Link (Silent Monitor)</h5>
          <code>http://{ips[0] || "localhost"}:{port}/?audio=off</code>
          <p className="hint">No audio playback; pure status tracking for handheld tablets.</p>
        </div>

        <div className="link-card">
          <h5>📟 Dedicated Prop Monitors</h5>
          <div className="prop-urls">
            {(config?.games || []).map((g) => (
              <div key={g.id} className="prop-url-item">
                <span>{g.name}:</span>
                <code>http://{ips[0] || "localhost"}:{port}/?game={g.id}</code>
              </div>
            ))}
          </div>
        </div>

        <div className="link-card">
          <h5>⚡ ESP32 Firmware Configuration</h5>
          <div className="config-item">
            <span>MQTT Broker IP:</span>
            <strong>{ips[0] || "192.168.1.X"}</strong>
          </div>
          <div className="config-item">
            <span>MQTT TCP Port:</span>
            <strong>{mqttPort}</strong>
          </div>
          <div className="config-item">
            <span>Root Topic:</span>
            <strong>{config?.rootTopic || "escaperoom"}</strong>
          </div>
        </div>
      </div>
    </div>
  );
}
