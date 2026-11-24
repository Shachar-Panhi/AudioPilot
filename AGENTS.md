# AudioPilot

Voice-controlled audio calls. Two browsers join a WebRTC call through a C++ backend. While a user holds push-to-talk (PTT), their audio is transcribed with Whisper and parsed into a voice command (e.g. `pitch -6`, `mixing rain`) instead of being sent to their partner.

- `backend/`: C++ server (Boost.Beast, libdatachannel, whisper.cpp). See [backend/AGENTS.md](backend/AGENTS.md).
- `frontend/`: React + TypeScript + Vite client. See [frontend/AGENTS.md](frontend/AGENTS.md).

## How the pieces connect

1. The frontend opens a WebSocket to the backend and gets a `session_id`. SDP and ICE signaling runs over that socket. The address is set in `backend/src/api/Listener.cpp` and `frontend/src/App.tsx`.
2. Call control is HTTP on the same address: `POST /api/call`, `/api/end_call`, `/api/ptt`.
3. The backend bridges two peers and forwards their Opus audio to each other.
4. While PTT is active, audio goes to `PTTHandler` → `STTHandler` (noise suppression + Whisper) → `CommandParser`. The result goes back over the WebSocket as an `stt_result` packet.

## Cross-cutting rules

- **Docs never copy values that can change.** That means tunables, timings, ports, versions and limits. Point to the constant or file that holds them, and follow this rule when editing any AGENTS.md.
- **Wire format lives in two places.** The structs in `backend/src/api/types.hpp` and the zod schemas in `frontend/src/types/index.ts` must match field for field. Change both together.
- **Comments are short, simple and to the point.** One line where possible. Say *why*, not what the code already says. Don't comment obvious code. Leave existing comments as they are unless asked.
- **Every new backend feature comes with Catch2 unit tests**, written in the same change. See [backend/AGENTS.md](backend/AGENTS.md#testing). The frontend has no tests, so check frontend changes by linting, building and running the app.

## Git

- Branches: `<issue-number>-<short-kebab-description>`, e.g. `25-implement-logic-for-transcribed-commands`. PRs target `main`.
- Commit messages: `<Type> - <Capitalised description>`, where Type is `Feat`, `Fix` or `Refactor`.
  e.g. `Fix - Merged the 3 PTT vectors into 1 vector with reference sizes`
