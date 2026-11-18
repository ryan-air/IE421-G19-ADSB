# FeedRecovery: TLA+ model of feed gap recovery

This folder holds a TLA+ model of the gap-detection and recovery protocol for the
MoldUDP64-style feed described in `README_UPDATES.md` §9.1 (framing) and §9.5
(gap detection and recovery). TLC checks it exhaustively for small constants.

`memory-model/` holds the GenMC (C11 memory model) checks of the two lock-free
rings; see `memory-model/README.md`.

| File | Purpose |
|---|---|
| `FeedRecovery.tla` | The spec: publisher, lossy network, retransmit server, snapshot server, subscriber, and the properties |
| `MC.cfg` | Correct model. All invariants and liveness must hold |
| `MC_NoSnapshot.cfg` | Deliberately broken variant (`EnableSnapshot = FALSE`). Must violate liveness |
| `MC_Reach.cfg` | Non-vacuity probe. Must violate `NeverSnapshot`, which shows the snapshot path is reachable |
| `run-tlc.sh` | Downloads the pinned TLA+ tools, runs all three configs, and checks each outcome against its expectation |

## What is modeled

- **Publisher.** It assigns sequence numbers `1..N` and multicasts packets `[first, count]`
  carrying `1..MaxBatch` consecutive messages, like a MoldUDP64 header with `seq` and `count`.
  A **heartbeat** is a packet with `count = 0` whose `first` is the next sequence number
  the publisher will use. Heartbeats are sent whenever the multicast channel is idle, and
  they continue after the last message.
- **Multicast network.** In-flight packets form a set, and any of them can be delivered
  next, so reordering is unrestricted. The network can **drop** a packet (shared budget
  `MaxLoss`) or **duplicate** one (budget `MaxDup`: the packet is delivered and a copy stays
  in flight).
- **Retransmit server.** It holds only the last `K` messages, `pubNext-K .. pubNext-1`. A
  request `(from, count)` gets back up to `MaxBatch` messages, or a **NAK** when `from` has
  already left the ring. Requests and replies are unicast, and both can be lost (they draw
  on the same `MaxLoss` budget).
- **Snapshot server.** It is reliable, like TCP. It returns the state "as of S", where
  `S = pubNext - 1` at the moment the snapshot is taken. The subscriber adopts the prefix up
  to `S`, discards buffered seqs `<= S`, and applies buffered seqs `> S` that are contiguous.
- **Subscriber.** It keeps `expected`, the set `buf` of seqs that arrived ahead of
  `expected`, and `known`, the highest seq it has learned exists from data or heartbeats.
  A gap is `known >= expected`. When a gap exists and no request or reply is outstanding,
  it sends a request for `expected .. min(buf)-1` (or up to `known`). This covers both the
  first request and later re-requests after a timeout. A NAK for the current `expected`
  switches it to snapshot mode.

## What is abstracted away

- **Payloads.** Message `i` carries the payload `i`, so the publisher's log is
  `<<1..pubNext-1>>`. The subscriber's "table" is `applied`, the sequence of ids it has
  applied. Loading a snapshot as of `S` appends `expected..S`: the snapshot counts as
  applying that prefix. Byte-identical reconstruction therefore becomes "`applied` equals
  the publisher's log".
- **Time.** There are no clocks. "The retransmit timer fired" becomes "nothing is
  outstanding", and "traffic stopped" becomes "the multicast channel is empty".
- **Not modeled.** Sessions, publisher restarts, the end-of-session marker (`0xFFFF`), byte
  layout, A/B lines, and multiple subscribers.
- **Set semantics.** Two identical packets in flight collapse into one. Duplicates are
  modeled explicitly by `DupMcast` instead.
- **Ring overwrite.** The server's ring is the window `pubNext-K..pubNext-1`, not an array
  that gets overwritten.

## Constants (all three configs use the same values)

`N = 5`, `K = 2`, `MaxBatch = 2`, `MaxLoss = 3`, `MaxDup = 2`.

With `K = 2 < N`, gaps older than the ring are reachable, so the snapshot path is exercised.

## Properties

Safety invariants, checked in every reachable state:

| Invariant | Meaning |
|---|---|
| `TypeOK` | Type correctness |
| `NoDoubleApply` | No message id appears twice in `applied` |
| `InOrder` | `applied[i+1] = applied[i] + 1`, so messages are applied strictly in order with no holes |
| `PrefixOfPublisher` | `applied` is a prefix of the publisher's log (counting a snapshot as applying `1..S`) |
| `ExpectedConsistent` | `expected = Len(applied) + 1` |
| `BufferAhead` | Every buffered seq is strictly greater than `expected` |
| `KnownSound` | The subscriber never learns of a seq that was not published |

Liveness properties:

- `EventuallyCaughtUp == <>(applied = <<1, ..., N>>)`
- `CaughtUpForever == <>[](applied = <<1, ..., N>>)`

## Fairness assumptions (and why)

`Spec == Init /\ [][Next]_vars /\ Fairness`. Weak fairness (WF) is placed on every action
of a correct party, and no fairness is placed on the adversary:

| Action | Fairness | Reason |
|---|---|---|
| `Publish` | WF | The publisher eventually sends all `N` messages |
| `Heartbeat` | WF | When traffic stops, heartbeats keep flowing. Without this, losing the *last* packet could never be detected |
| `RecvMcastAny` | WF | A packet left in flight is eventually delivered |
| `SendRequest` | WF | The retransmit timeout eventually fires |
| `ServeRequest`, `RecvReply` | WF | The retransmit server answers, and the reply is eventually delivered |
| `Snapshot` | WF | The TCP snapshot eventually completes |
| `DropMcast`, `DupMcast`, `DropRequest`, `DropReply` | none | These are the adversary. Their finite budgets mean **loss eventually ceases** in every behavior |

Weak fairness is enough here. Each fair action, once enabled, stays enabled until it is
taken or until another step makes it unnecessary (for example, a gap filled by a
reordered packet disables `SendRequest`).

Aggregate WF on `RecvMcastAny`, instead of WF for each packet, cannot starve a packet:

- each non-duplicating delivery removes a packet;
- duplication is bounded;
- heartbeats are only added when the channel is empty.

Strong fairness is not needed, and per-packet fairness would only make liveness checking
slower.

## How to run

```sh
spec/run-tlc.sh        # needs Java 11+ and curl; works from any directory
```

The script downloads `tla2tools.jar` **v1.7.4**, the latest stable release, dated
2024-08-08. v1.8.0 exists but is a prerelease published in Nov 2026, so it fails the
"at least a year old" pin rule. The jar comes from
`github.com/tlaplus/tlaplus/releases` and goes into `spec/.tools/`. The script verifies its
SHA-256 against
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.

It then runs each config and compares the result with what that config should produce:

| Config | Expected TLC exit code | Meaning of that exit code |
|---|---|---|
| `MC.cfg` | 0 | No error |
| `MC_NoSnapshot.cfg` | 13 | Liveness violation |
| `MC_Reach.cfg` | 12 | Invariant violation |

The script exits nonzero if any config deviates from its expected result, in particular
on any violation in the correct model. Logs are written to `spec/out/*.out`.

## Results

These are actual runs: TLC 2.19 (tla2tools v1.7.4), Java 24, 8 workers, macOS.

| Config | Result | States generated | Distinct states | Depth | Time |
|---|---|---|---|---|---|
| `MC.cfg` | **PASS**: no invariant or liveness violation | 1,457,453 | 211,522 | 23 | 23 s |
| `MC_NoSnapshot.cfg` | **FAIL as expected**: "Temporal properties were violated" (`EventuallyCaughtUp`) | 269,076 | 59,822 | n/a | 3 s |
| `MC_Reach.cfg` | **FAIL as expected**: "Invariant NeverSnapshot is violated" (8-state trace) | 7,554 | 3,291 | 8 | <1 s |

The whole `run-tlc.sh` run, including the download, took about 30 s of wall time.

### Broken variant counterexample (`MC_NoSnapshot.cfg`)

TLC found a 13-state lasso that loops back to state 9:

1. Packets `[1,2]` and `[3,2]` are published.
2. `[3,2]` arrives first, so `buf = {3,4}`. The subscriber requests `(from 1, count 2)`.
3. `[1,2]` is dropped. `[5,1]` is published and delivered (with a duplicate copy, which is
   then dropped), so `buf = {3,4,5}` and `pubNext = 6`.
4. The ring now holds only seqs 4 and 5, so the server answers with a **NAK**.
5. With no snapshot fallback, the subscriber ignores the NAK. A heartbeat `[6,0]` arrives,
   the subscriber re-requests from 1, gets another NAK, and so on forever. The loop runs
   ServeRequest, RecvReply, Heartbeat, RecvMcastAny, SendRequest.

`expected` stays at 1 forever. Every fair action keeps being taken, and the drop budget is
not exhausted, yet `EventuallyCaughtUp` fails. The same model with the snapshot enabled
(`MC.cfg`) passes, so the snapshot fallback is exactly what is needed for liveness.

### Further sanity check (not kept as a config)

As a mutation test, the snapshot was changed to append the *full* prefix `1..S` on top of
what had already been applied. TLC reported `Invariant NoDoubleApply is violated` after 9
states: receive 1..2, NAK, snapshot. This shows that the safety invariants also have
teeth.

## GitHub Actions job (ready to paste)

Add this under `jobs:` in a workflow. This folder does not edit any workflow file.

```yaml
  tlc-feed-recovery:
    name: TLA+ model check (FeedRecovery)
    runs-on: ubuntu-latest
    timeout-minutes: 15
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: '21'
      - name: Cache tla2tools.jar
        uses: actions/cache@v4
        with:
          path: spec/.tools
          key: tla2tools-v1.7.4
      - name: Run TLC
        run: bash spec/run-tlc.sh
      - name: Upload TLC logs
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: tlc-logs
          path: spec/out/
```
