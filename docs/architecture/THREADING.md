# Threading model

How pp-ledger processes use threads around the AMP stack and server state.
Code wins if this disagrees; see `src/network/LedgerAmpRuntime.*`,
`src/client/AmpLedgerTransport.*`, `src/server/Server.*`.

## Model

Two kinds of thread own everything; other threads hand them work.

| Thread | Owns | Does |
|--------|------|------|
| **Network (pump) thread** — `LedgerAmpRuntime` | The AMP stack: links, channel mux, channel sessions | Receives and sends datagrams, runs posted io work, fires amp timers |
| **Server thread** — per role (target, see below) | Chain / ledger / role state | Handles requests one at a time, runs role duties (refresh, sync, block production) |

Messages flow through queues between them:

```
peer ──► pump thread ──(request queue)──► server thread
peer ◄── pump thread ◄──(reply: PostToIo)── server thread
```

- **Inbound:** the pump thread decodes a request and posts it for handling
  (`AmpLedgerServer` `post_worker` hook); the reply goes back to the pump thread
  with `PostToIo`.
- **Outbound (server-initiated):** the caller posts the call to the io lane and
  waits for its result (`AmpLedgerTransport::roundTrip`). The caller never drives
  the stack itself. From the caller's side this is an ordinary blocking call.

## Rules

1. **One driver.** Only the pump thread calls `MeshRuntime::Drive` / `Pump` /
   `Tick` (amp's exclusive-driver contract). Tests may instead drive from the
   test thread, which is then the only driver.
2. **No off-thread link work.** Other threads do not call into links, channel
   muxes or sessions per request. They use `LedgerAmpRuntime::post()` and wait.
   `links()` / `runtime()` are for setup (binding handlers) only.
3. **Never block the pump thread.** A blocking `roundTrip` from the pump thread
   would wait on itself; it returns an error instead.
4. **Io-lane callbacks own their state.** Amp callbacks can fire after a caller
   has timed out and returned, so they hold shared state (`RoundTripCall`), never
   references into the caller's stack frame.
5. **Ledger and Chain are single-threaded.** They take no locks; their owner
   serializes access.

## Transport modes

`AmpLedgerTransport` supports two ways of reaching the driver:

- **Threaded** (`AmpLedgerTransport(LedgerAmpRuntime&, peer)`): servers, pp-client,
  pp-http. `roundTrip` posts, then waits on a condition variable. The io-lane amp
  timer ends the call at `timeout`; the waiter adds a short grace period in case
  the pump thread stalls.
- **Caller-driven** (`AmpLedgerTransport(MeshRuntime&, peer, drive)`): unit tests
  on a virtual clock. `roundTrip` posts, then runs `drive` on the calling thread
  until done, bounded by wall time.

## Status and plan

| Phase | Scope | State |
|-------|-------|-------|
| 1 | Pump thread owns the AMP stack; outbound calls post and wait | Done |
| 2 | Single server thread per role; inbound requests queued to it (Beacon first, then Relay and Miner) | Planned |
| 3 | Handlers that call out (`hTxAdd` forward, `hBlockGet` sync-on-miss, relay forwarding) reply later from a completion instead of blocking the server thread | Planned |
| 4 | Background sync as completions too, so the server thread never blocks | Planned |

Until phase 2 lands, RPC handlers still run on `Server`'s `WorkerPool` next to
each role's `runLoop` thread, so role state is shared across threads.

Phase 3 matters for liveness: while a server thread blocks on an outbound call it
handles nothing else, and two miners forwarding to each other at a slot boundary
would each wait for the other until timeout.

Planned additions with phase 2: a bounded request queue that answers "busy" when
full, dropping requests older than the client's timeout, and role timers
(`refresh`, slot ticks) as queue events instead of sleeps.
