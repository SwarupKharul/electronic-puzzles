# 🎵 Audio Assets for Game 1 (Knock Puzzle)

Drop sound effect files for Game 1 into this directory (`audio/game1/`).

### Supported Audio Formats:
`.mp3`, `.wav`, `.ogg`, `.m4a`

### Filename Conventions & Event Mapping:
- `start.mp3`    ➔ Played when Game 1 starts (`STARTED` event)
- `failed.mp3`   ➔ Played when rhythm pattern is incorrect (`FAILED` event)
- `complete.mp3` / `success.mp3` ➔ Played when puzzle is solved (`COMPLETED` event)
- `stop.mp3`     ➔ Played when Game Master halts the puzzle (`STOPPED` event)
- `reset.mp3` / `ready.mp3`   ➔ Played when prop is reset to armed standby (`RESET` / `READY` event)

*Note: The control server scans this directory on startup. If any audio file is missing, the web dashboard automatically synthesizes real-time chimes and fanfares via the Web Audio API.*
