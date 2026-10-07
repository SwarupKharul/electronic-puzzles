# 🎵 Audio Assets for Game 2 (4-RFID Dolls Puzzle)

Drop sound effect files for Game 2 into this directory (`audio/game2/`).

### Supported Audio Formats:
`.mp3`, `.wav`, `.ogg`, `.m4a`

### Filename Conventions & Event Mapping:
- `start.mp3`    ➔ Played when Game 2 starts (`STARTED` event)
- `failed.mp3`   ➔ Played when 4 dolls are placed in wrong order (`FAILED` event)
- `complete.mp3` / `success.mp3` ➔ Played when all 4 dolls match the secret pattern (`COMPLETED` event)
- `stop.mp3`     ➔ Played when Game Master halts the puzzle (`STOPPED` event)
- `reset.mp3` / `ready.mp3`   ➔ Played when prop is reset to armed standby (`RESET` / `READY` event)

*Note: The control server scans this directory on startup. If any audio file is missing, the web dashboard automatically synthesizes real-time chimes and fanfares via the Web Audio API.*
