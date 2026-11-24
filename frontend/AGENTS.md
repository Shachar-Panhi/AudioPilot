# Frontend

A single-page test console for AudioPilot. Connect to the backend, call another session by ID, and hold Ctrl+Space to send a voice command. Everything shows up in a scrolling log.

Stack: React, TypeScript, Vite, Tailwind, axios (HTTP), zod (packet validation). There's no router, state library or test framework.

## Commands

Run from `frontend/`:

```sh
npm install
npm run dev      # Vite dev server. The backend must already be running
npm run build    # tsc -b && vite build. This is the type-check
npm run lint     # oxlint (react, typescript, oxc plugins)
```

Before finishing, `npm run lint` and `npm run build` must both pass.

To try a change by hand, open two tabs, press **Connect to Server** in both, paste one tab's session ID into the other, and press **Call**.

## Layout

| File | Role |
|---|---|
| `src/hooks/useWebRTC.ts` | All connection logic: WebSocket signaling, `RTCPeerConnection`, mic capture, HTTP calls, PTT, the message log. |
| `src/App.tsx` | UI and layout. Passes the hardcoded backend URLs into `useWebRTC`. |
| `src/components/MessageList.tsx` | The log view. Colours each entry by its `MessageType`. |
| `src/types/index.ts` | zod schemas for every packet, plus the `LogMessage` types. |
| `src/main.tsx`, `src/index.css` | Entry point. `index.css` only has `@import "tailwindcss"`. |

Leftovers from the Vite template, safe to ignore: `src/App.css` (empty, not imported), `src/assets/*`, `README.md`.

## Talking to the backend

The protocol is defined in `src/types/index.ts`, which must match `backend/src/api/types.hpp` field for field. Change both together.

| Direction | Channel | Payload |
|---|---|---|
| server → client | WebSocket | `{session_id}` on connect, SDP offer, ICE candidates, `api_response` (`action`: `match` / `disconnect`), `stt_result` |
| client → server | WebSocket | SDP answer, ICE candidates (both include `session_id`) |
| client → server | HTTP POST | `/api/call` `{session_id, target_session_id}`, `/api/end_call` `{session_id}`, `/api/ptt` `{session_id, active}` |

HTTP replies hold a `status` field, and `'success'` means it worked.

### Matching incoming packets

`ws.onmessage` tries each zod schema in turn and stops at the first match. **The order matters**: zod drops unknown keys, so `ConnectionPacketSchema` (`{session_id}`) would also match an SDP or ICE packet. It has to stay last.

When adding a packet type:
- give it a `type: z.literal('...')` field, like `stt_result`
- check it before the SDP, ICE and connection checks
- add the matching struct on the backend

## WebRTC details

- **The backend sends the offer and the browser answers.** The browser never creates an offer.
- `initCall()` is idempotent. It's called on `match` and again when the offer arrives, and it waits for the mic before adding tracks.
- ICE candidates that arrive before `setRemoteDescription` go into `iceCandidateQueueRef` and are added after it. Keep that order.
- **The SDP answer is rewritten on purpose** to pin the Opus settings: frame size, bitrate, channels, sample rate, CBR and DTX. The sender's `maxBitrate` is capped as well. The backend's Opus decoding (`OpusTranscoder`) and PTT packet handling depend on these settings, so change both sides together.
- The `getUserMedia` constraints match those Opus settings.

## PTT

- Hold **Ctrl+Space** to start. Releasing either key stops it. `e.repeat` is ignored.
- On release, the stop request is delayed by `PTT_RELEASE_TAIL_MS` so the end of the last word reaches the server. Pressing again inside that window cancels the stop and keeps the same recording going.
- If `/api/ptt` fails on start, PTT is turned back off in the UI.

## Code conventions

- **Long-lived state lives in `useWebRTC`.** Components get state and actions from the hook and keep only local UI state (input text, the "Copied" flag).
- **Every value an async handler reads must also be kept in a ref** (`sessionIdRef`, `isInCallRef`, `pttActiveRef`, …), because WebSocket and timer callbacks would otherwise see stale state. When adding such state, update the ref and the `useState` together.
- Wrap functions the hook returns or uses in `useCallback` with complete dependency lists. React Compiler isn't enabled, so nothing memoises for you.
- Guard `setState` after `await` with `isMountedRef`. Before closing the WebSocket, set its `on*` handlers to `null`, as `connectToServer` and the unmount cleanup do.
- Show results to the user with `appendLog(text, type)`. Use `'sent'` for the user's actions, `'received'` for what the partner or server produced (like a recognised command), and `'system'` for status and errors. Errors are logged, never thrown, and never shown with `alert`.
- Handle HTTP errors the same way everywhere: `axios.isAxiosError(err) ? err.response?.data?.status || err.message : String(err)`.
- Parse every incoming packet with its zod schema. Don't use `as` casts or `any`.

## TypeScript and tooling

- `verbatimModuleSyntax` is on, so type-only imports must use `import type` / `{ type X }`.
- `erasableSyntaxOnly` is on, so `enum`, `namespace` and constructor parameter properties aren't allowed. Use string-literal unions instead, like `MessageType`.
- `noUnusedLocals` and `noUnusedParameters` turn unused code into build errors.
- No Prettier is configured, so match the formatting of the surrounding code.
- **Tailwind is configured in CSS** (`@import "tailwindcss"` in `index.css`, through `@tailwindcss/postcss`) and finds classes on its own. `tailwind.config.js` is a leftover that isn't loaded, so editing it does nothing. Style with utility classes in JSX, and don't add CSS files.
