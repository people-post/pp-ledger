# Threading model

How pp-ledger processes use threads around the AMP stack and server state.
Code wins if this disagrees; see `src/network/LedgerAmpRuntime.*`,
`src/client/AmpLedgerTransport.*`, `src/server/Server.*`.

## Model

Two kinds of thread own everything; other threads hand them work.

| Thread | Owns | Does |
|--------|------|------|
| **Network (pump) thread** — `LedgerAmpRuntime` | The AMP stack: links, channel mux, channel sessions | Receives and sends datagrams, runs posted io work, fires amp timers |
| **Server thread** — the role's `runLoop` thread | Chain / ledger / role state | Runs role duties (refresh, sync, block production) and handles requests one at a time in between |

Messages flow through queues between them:

```
peer ──► pump thread ──(request queue)──► server thread
peer ◄── pump thread ◄──(reply: PostToIo)── server thread
```

- **Inbound:** the pump thread receives a request and pushes it onto the role's
  `RequestQueue` (`AmpLedgerServer::BindAsync`). The server thread handles queued
  requests in `Server::serveRequestsFor()`, which role run loops call in place of
  sleeping between duties. The reply is posted back to the pump thread.
- **Back-pressure:** the queue is bounded; when full, a request gets an immediate
  "busy" reply. A request that waited longer than the client's read timeout gets
  an "expired" reply without running the handler. On stop, pending requests get
  a "stopping" reply.
- **Outbound (server-initiated):** the caller posts the call to the io lane. It
  either waits for the result (`AmpLedgerTransport::roundTrip`, an ordinary
  blocking call) or gets it in a callback on the io lane (`roundTripAsync`,
  `Client::*Async`). The caller never drives the stack itself.
- **Deferred replies:** a handler that needs another server keeps the request's
  `reply` (`Server::handleDeferred`), starts an async call, and returns. The
  result hops back to the server thread (`postToServerThread` /
  `completeOnServerThread`), which updates state and replies. Meanwhile the
  server thread keeps serving other requests.

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
| 2 | Single server thread per role; inbound requests queued to it | Done |
| 3 | Handlers that call out reply later instead of blocking the server thread: relay forwards (block add, register, miner list) and miner tx forwarding go async; block gets beyond the tip wait for the run loop's next sync | Done |
| 4 | Background sync as completions too, so the server thread never blocks | Planned |

Role duties still make blocking calls (sync from the beacon, calibration, block
broadcast, miner-list refresh); queued requests wait while they run. Phase 4
turns those into completions too.

Still open: role timers (`refresh`, slot ticks) as queue events rather than the
fixed serve budget between duties.
