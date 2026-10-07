// =================================================================================
// Escape Room Control - Robust Audio Engine & Synthesizer Fallback
// =================================================================================
// Supports:
// 1. Zero-latency Web Audio API buffer playback for custom MP3/WAV audio files.
// 2. Immediate HTML5 Audio element fallback.
// 3. Built-in Web Audio Oscillator Synthesizer for instant FX when files are missing
//    (e.g., chime, fanfare, buzzer, reset sweeps).
// 4. Guaranteed browser autoplay unlocking via synchronous user gestures.
// =================================================================================

export const EVENT_SOUND = {
  READY: "reset",
  START: "start",
  STARTED: "start",
  FAILED: "failed",
  SUCCESS: "success",
  COMPLETE: "complete",
  COMPLETED: "complete",
  STOP: "stop",
  STOPPED: "stop",
  RESET: "reset"
};

export class AudioPlayer {
  constructor() {
    this.ctx = null;
    this.buffers = new Map();         // url -> AudioBuffer
    this.loadingPromises = new Map(); // url -> Promise<AudioBuffer>
    this.audioElements = new Map();   // url -> HTMLAudioElement
    this.muted = false;
    this.masterVolume = 1.0;
    this.isUnlocked = false;
  }

  // Ensure AudioContext exists and is running
  getAudioContext() {
    if (!this.ctx && typeof window !== "undefined") {
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      if (AudioCtx) {
        this.ctx = new AudioCtx();
      }
    }
    return this.ctx;
  }

  // Ensure AudioContext is actively in the 'running' state
  async ensureRunning() {
    const ctx = this.getAudioContext();
    if (!ctx) return null;
    if (ctx.state === "suspended") {
      try {
        await ctx.resume();
        console.log("🔊 [Audio] AudioContext resumed successfully (state: running)");
      } catch (err) {
        console.warn("⚠️ [Audio] AudioContext resume error:", err);
      }
    }
    return ctx;
  }

  // Synchronously and asynchronously unlock the browser audio context on user interaction
  async unlock(urls = []) {
    try {
      const ctx = await this.ensureRunning();
      if (ctx) {
        // Play silent 1-sample buffer to satisfy strict browser autoplay policies
        const silentBuffer = ctx.createBuffer(1, 1, 22050);
        const source = ctx.createBufferSource();
        source.buffer = silentBuffer;
        source.connect(ctx.destination);
        source.start(0);
      }

      this.isUnlocked = true;

      // Preload provided sound URLs
      if (Array.isArray(urls) && urls.length > 0) {
        urls.filter(Boolean).forEach((url) => {
          this.preload(url).catch((err) => console.warn(`Preload warning for ${url}:`, err));
        });
      }

      return true;
    } catch (err) {
      console.warn("Audio unlock warning:", err);
      return false;
    }
  }

  // Pre-fetch and decode audio file into memory for instant zero-latency playback
  async preload(url) {
    if (!url) return null;
    if (this.buffers.has(url)) return this.buffers.get(url);
    if (this.loadingPromises.has(url)) return this.loadingPromises.get(url);

    const loadPromise = (async () => {
      try {
        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status} loading ${url}`);
        const arrayBuffer = await response.arrayBuffer();

        const ctx = this.getAudioContext();
        if (ctx) {
          // Robust decodeAudioData supporting both promise and callback styles
          const decoded = await new Promise((resolve, reject) => {
            try {
              const res = ctx.decodeAudioData(
                arrayBuffer.slice(0),
                (buf) => resolve(buf),
                (err) => reject(err)
              );
              if (res && typeof res.then === "function") {
                res.then(resolve).catch(reject);
              }
            } catch (e) {
              reject(e);
            }
          });
          this.buffers.set(url, decoded);
          return decoded;
        }
      } catch (err) {
        console.warn(`Web Audio decode failed for ${url}, fallback to HTMLAudioElement:`, err);
        // Fallback: create HTML5 Audio element
        const audio = new Audio(url);
        audio.preload = "auto";
        this.audioElements.set(url, audio);
      }
      return null;
    })();

    this.loadingPromises.set(url, loadPromise);
    return loadPromise;
  }

  // Play audio file by URL, or fallback to real-time Web Audio Synthesizer FX
  async play(gameId, url, eventName) {
    if (this.muted || (eventName && eventName.toUpperCase() === "HEARTBEAT")) {
      return;
    }

    // Ensure AudioContext is running before trying to play anything
    const ctx = await this.ensureRunning();

    if (!url) {
      // No custom sound file -> use built-in synthesizer FX
      console.log(`🎵 [Audio] Playing synth sound for [${gameId}] event: ${eventName}`);
      await this.playSynthSound(eventName || "CLICK");
      return;
    }

    try {
      // 1. Try playing from pre-decoded Web Audio buffer (Highest performance)
      let buffer = this.buffers.get(url);
      if (!buffer && this.loadingPromises.has(url)) {
        buffer = await this.loadingPromises.get(url);
      }

      if (!buffer) {
        // If not preloaded, fetch and decode immediately
        buffer = await this.preload(url);
      }

      if (ctx && buffer) {
        const source = ctx.createBufferSource();
        const gainNode = ctx.createGain();
        source.buffer = buffer;
        gainNode.gain.value = this.masterVolume;
        source.connect(gainNode);
        gainNode.connect(ctx.destination);
        source.start(0);
        console.log(`🔊 [Audio] Playing buffer sound: ${url} (vol: ${this.masterVolume})`);
        return;
      }

      // 2. Fallback to HTML5 Audio element
      let audio = this.audioElements.get(url);
      if (!audio) {
        audio = new Audio(url);
        this.audioElements.set(url, audio);
      }
      audio.currentTime = 0;
      audio.volume = this.masterVolume;
      console.log(`🔊 [Audio] Playing via HTML5 Audio element: ${url}`);
      await audio.play();
    } catch (err) {
      console.warn(`⚠️ [Audio] Playback failed for [${gameId}] ${url}, falling back to synth:`, err);
      await this.playSynthSound(eventName || "CLICK");
    }
  }

  // Built-in Synthesizer Sound Effects (Works even if audio files are missing or unsupported)
  async playSynthSound(eventName) {
    if (this.muted || (eventName && eventName.toUpperCase() === "HEARTBEAT")) return;
    try {
      const ctx = await this.ensureRunning();
      if (!ctx) return;

      const now = ctx.currentTime + 0.02; // Small 20ms lookahead prevents glitch
      const masterGain = ctx.createGain();
      // Master synth volume at 0.85 for clear audibility
      masterGain.gain.setValueAtTime(0.85 * this.masterVolume, now);
      masterGain.connect(ctx.destination);

      const type = (eventName || "").toUpperCase();

      if (type === "STARTED" || type === "START") {
        // 🎺 Rising Start Fanfare (C4 -> E4 -> G4)
        [261.63, 329.63, 392.00].forEach((freq, i) => {
          const osc = ctx.createOscillator();
          const gain = ctx.createGain();
          osc.type = "sine";
          osc.frequency.setValueAtTime(freq, now + i * 0.12);

          gain.gain.setValueAtTime(0.001, now + i * 0.12);
          gain.gain.exponentialRampToValueAtTime(0.4, now + i * 0.12 + 0.02);
          gain.gain.exponentialRampToValueAtTime(0.001, now + i * 0.12 + 0.35);

          osc.connect(gain);
          gain.connect(masterGain);
          osc.start(now + i * 0.12);
          osc.stop(now + i * 0.12 + 0.35);
        });
      } else if (type === "FAILED" || type === "FAIL") {
        // ❌ Low Failure Buzzer (Dissonant descending buzz)
        [220, 207.65].forEach((freq) => {
          const osc = ctx.createOscillator();
          const gain = ctx.createGain();
          osc.type = "sawtooth";
          osc.frequency.setValueAtTime(freq, now);
          osc.frequency.linearRampToValueAtTime(freq * 0.6, now + 0.5);

          gain.gain.setValueAtTime(0.3, now);
          gain.gain.exponentialRampToValueAtTime(0.001, now + 0.5);

          osc.connect(gain);
          gain.connect(masterGain);
          osc.start(now);
          osc.stop(now + 0.5);
        });
      } else if (type === "COMPLETED" || type === "COMPLETE" || type === "SUCCESS") {
        // 🏆 Victory Chime (Rich Major Chords)
        const notes = [523.25, 659.25, 783.99, 1046.50, 1318.51];
        notes.forEach((freq, i) => {
          const osc = ctx.createOscillator();
          const gain = ctx.createGain();
          osc.type = "sine";
          osc.frequency.setValueAtTime(freq, now + i * 0.14);

          gain.gain.setValueAtTime(0.001, now + i * 0.14);
          gain.gain.exponentialRampToValueAtTime(0.5, now + i * 0.14 + 0.03);
          gain.gain.exponentialRampToValueAtTime(0.001, now + i * 0.14 + 0.8);

          osc.connect(gain);
          gain.connect(masterGain);
          osc.start(now + i * 0.14);
          osc.stop(now + i * 0.14 + 0.8);
        });
      } else if (type === "STOPPED" || type === "STOP") {
        // ⏹ Stop warning tone
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.type = "square";
        osc.frequency.setValueAtTime(350, now);
        osc.frequency.setValueAtTime(250, now + 0.15);

        gain.gain.setValueAtTime(0.2, now);
        gain.gain.exponentialRampToValueAtTime(0.001, now + 0.35);

        osc.connect(gain);
        gain.connect(masterGain);
        osc.start(now);
        osc.stop(now + 0.35);
      } else if (type === "RESET") {
        // ⟲ Reset sweep (whoosh down then up)
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.type = "sine";
        osc.frequency.setValueAtTime(600, now);
        osc.frequency.exponentialRampToValueAtTime(200, now + 0.15);
        osc.frequency.exponentialRampToValueAtTime(800, now + 0.35);

        gain.gain.setValueAtTime(0.25, now);
        gain.gain.exponentialRampToValueAtTime(0.001, now + 0.4);

        osc.connect(gain);
        gain.connect(masterGain);
        osc.start(now);
        osc.stop(now + 0.4);
      } else {
        // Neutral clean click
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.type = "sine";
        osc.frequency.setValueAtTime(600, now);

        gain.gain.setValueAtTime(0.25, now);
        gain.gain.exponentialRampToValueAtTime(0.001, now + 0.08);

        osc.connect(gain);
        gain.connect(masterGain);
        osc.start(now);
        osc.stop(now + 0.08);
      }
    } catch (e) {
      console.warn("Synth sound exception:", e);
    }
  }

  setMuted(muted) {
    this.muted = muted;
  }

  setVolume(volume) {
    this.masterVolume = Math.max(0, Math.min(1, volume));
  }
}
