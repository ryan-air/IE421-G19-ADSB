---------------------------- MODULE FeedRecovery ----------------------------
(***************************************************************************)
(* Gap detection and recovery for a sequenced, MoldUDP64-style market-data *)
(* feed (README_UPDATES.md sections 9.1 and 9.5).                          *)
(*                                                                         *)
(* Actors                                                                  *)
(*   - Publisher: assigns sequence numbers 1..N and multicasts packets     *)
(*     [first |-> s, count |-> c] carrying messages s..s+c-1 (1 <= c <=    *)
(*     MaxBatch).  A heartbeat is a packet with count = 0 whose `first` is *)
(*     the next sequence number the publisher will use.                    *)
(*   - Multicast network: may drop (budget MaxLoss), duplicate (budget     *)
(*     MaxDup) and reorder packets.  Reordering is free: in-flight packets *)
(*     are a set and any element may be delivered next.                    *)
(*   - Retransmit server: holds only the last K published messages.  It   *)
(*     answers a unicast request (from, count) with up to MaxBatch         *)
(*     messages, or with a NAK if `from` has already fallen out of the     *)
(*     ring.  Requests and replies can be lost (same MaxLoss budget).      *)
(*   - Snapshot server (TCP, reliable): returns the publisher's state "as  *)
(*     of seq S", where S is the publisher's latest sequence number.       *)
(*   - Subscriber: applies messages strictly in order, buffers anything    *)
(*     that arrives ahead of `expected`, requests retransmission of gaps,  *)
(*     re-requests after a timeout, and falls back to a snapshot on NAK.   *)
(*                                                                         *)
(* Abstractions                                                            *)
(*   - Message i's payload is just its id i, so the publisher's log is the *)
(*     sequence <<1, 2, ..., pubNext-1>> and needs no variable of its own. *)
(*   - The subscriber's "table" is the sequence `applied` of message ids   *)
(*     it has applied.  Loading a snapshot as of S appends the ids         *)
(*     expected..S, i.e. the snapshot counts as applying that prefix.      *)
(*   - Timers are abstracted: "the retransmit timeout fired" is modelled   *)
(*     as "no request or reply is outstanding" (it was lost or answered).  *)
(*   - Sessions, end-of-session packets, byte layout and the 1 s heartbeat *)
(*     period are not modelled.  A heartbeat is sent only while the        *)
(*     multicast channel is idle (i.e. "when traffic stops").              *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    N,              \* number of messages the publisher will ever send
    K,              \* retransmit ring capacity (last K messages)
    MaxBatch,       \* max messages per packet / per retransmit reply
    MaxLoss,        \* total packets (mcast, request or reply) the network may drop
    MaxDup,         \* total multicast packets the network may duplicate
    EnableSnapshot  \* FALSE = deliberately broken variant without snapshot fallback

ASSUME /\ N \in Nat \ {0}
       /\ K \in Nat \ {0}
       /\ MaxBatch \in Nat \ {0}
       /\ MaxLoss \in Nat
       /\ MaxDup \in Nat
       /\ EnableSnapshot \in BOOLEAN

VARIABLES
    pubNext,    \* publisher: next sequence number to assign (1..N+1)
    mcast,      \* set of multicast packets in flight
    reqs,       \* set of unicast retransmit requests in flight
    reps,       \* set of unicast retransmit replies in flight
    lossLeft,   \* remaining drop budget
    dupLeft,    \* remaining duplication budget
    expected,   \* subscriber: next sequence number it needs to apply
    applied,    \* subscriber: sequence of applied message ids (its state)
    buf,        \* subscriber: buffered sequence numbers received ahead of `expected`
    known,      \* subscriber: highest sequence number it knows was published
    mode        \* subscriber: "live" or "snapshot" (waiting for snapshot)

vars == <<pubNext, mcast, reqs, reps, lossLeft, dupLeft,
          expected, applied, buf, known, mode>>

Seqs     == 1..N
Min(S)   == CHOOSE x \in S : \A y \in S : x <= y
Max2(a, b) == IF a >= b THEN a ELSE b
Min2(a, b) == IF a <= b THEN a ELSE b

Packet  == [first : 1..(N+1), count : 0..MaxBatch]          \* count 0 = heartbeat
Request == [from : Seqs, count : 1..N]
Reply   == [kind : {"data", "nak"}, first : Seqs, count : 0..MaxBatch]

\* The publisher's log at any moment: message i has payload i.
PubLog == [i \in 1..(pubNext - 1) |-> i]

-----------------------------------------------------------------------------
(* Subscriber helpers *)

\* Given the current log `base`, the next needed seq e and a set B of
\* received-but-unapplied seqs, apply the longest contiguous run
\* e, e+1, ..., m that is in B, and keep only what is still ahead of the
\* new `expected` (m+1 is not in B, so that is {s \in B : s > m+1}).
Advance(base, e, B) ==
    LET m == CHOOSE x \in (e - 1)..N :
                 /\ \A s \in e..x : s \in B
                 /\ (x + 1) \notin B
    IN /\ applied'  = base \o [i \in 1..(m - e + 1) |-> e + i - 1]
       /\ expected' = m + 1
       /\ buf'      = {s \in B : s > m + 1}

\* Process messages first..first+cnt-1 (from multicast or a retransmit
\* reply): anything below `expected` is a duplicate and is discarded,
\* everything else is buffered, then the contiguous prefix is applied.
Accept(first, cnt) ==
    LET fresh == {s \in first..(first + cnt - 1) : s >= expected}
    IN Advance(applied, expected, buf \cup fresh)

\* A gap exists when the subscriber knows of a message it has not applied.
Gap == known >= expected

-----------------------------------------------------------------------------
Init ==
    /\ pubNext  = 1
    /\ mcast    = {}
    /\ reqs     = {}
    /\ reps     = {}
    /\ lossLeft = MaxLoss
    /\ dupLeft  = MaxDup
    /\ expected = 1
    /\ applied  = <<>>
    /\ buf      = {}
    /\ known    = 0
    /\ mode     = "live"

(* ---------------- Publisher ---------------- *)

\* Publish the next b consecutive messages in one packet.
Publish ==
    /\ pubNext <= N
    /\ \E b \in 1..Min2(MaxBatch, N - pubNext + 1) :
          /\ mcast'   = mcast \cup {[first |-> pubNext, count |-> b]}
          /\ pubNext' = pubNext + b
    /\ UNCHANGED <<reqs, reps, lossLeft, dupLeft, expected, applied, buf, known, mode>>

\* Heartbeat: empty packet announcing the next seq to be used.  Sent when
\* the channel is idle ("traffic stopped"), forever (also after N).
Heartbeat ==
    /\ mcast = {}
    /\ mcast' = {[first |-> pubNext, count |-> 0]}
    /\ UNCHANGED <<pubNext, reqs, reps, lossLeft, dupLeft, expected, applied, buf, known, mode>>

(* ---------------- Multicast network ---------------- *)

DropMcast ==
    /\ lossLeft > 0
    /\ \E p \in mcast : mcast' = mcast \ {p}
    /\ lossLeft' = lossLeft - 1
    /\ UNCHANGED <<pubNext, reqs, reps, dupLeft, expected, applied, buf, known, mode>>

\* Deliver packet p to the subscriber.  If dup is TRUE the network keeps a
\* copy in flight, so the same packet will be delivered again later.
RecvMcast(p, dup) ==
    /\ p \in mcast
    /\ IF dup THEN /\ dupLeft > 0
                   /\ dupLeft' = dupLeft - 1
                   /\ mcast'   = mcast
              ELSE /\ mcast'   = mcast \ {p}
                   /\ UNCHANGED dupLeft
    /\ IF p.count = 0
         THEN /\ known' = Max2(known, p.first - 1)        \* heartbeat
              /\ UNCHANGED <<expected, applied, buf>>
         ELSE /\ known' = Max2(known, p.first + p.count - 1)
              /\ Accept(p.first, p.count)
    /\ UNCHANGED <<pubNext, reqs, reps, lossLeft, mode>>

RecvMcastAny == \E p \in mcast : RecvMcast(p, FALSE)
DupMcast     == \E p \in mcast : RecvMcast(p, TRUE)

(* ---------------- Retransmit path ---------------- *)

\* Subscriber sends (or, after a timeout, re-sends) a retransmit request
\* for expected..(first buffered seq - 1), or up to `known` if nothing is
\* buffered (gap revealed by a heartbeat).
SendRequest ==
    /\ mode = "live"
    /\ Gap
    /\ reqs = {} /\ reps = {}                 \* nothing outstanding: timeout
    /\ LET hi == IF buf # {} THEN Min(buf) - 1 ELSE known
       IN reqs' = {[from |-> expected, count |-> hi - expected + 1]}
    /\ UNCHANGED <<pubNext, mcast, reps, lossLeft, dupLeft, expected, applied, buf, known, mode>>

DropRequest ==
    /\ lossLeft > 0
    /\ \E r \in reqs : reqs' = reqs \ {r}
    /\ lossLeft' = lossLeft - 1
    /\ UNCHANGED <<pubNext, mcast, reps, dupLeft, expected, applied, buf, known, mode>>

\* Retransmit server: its ring holds seqs Max(1, pubNext-K)..pubNext-1.
ServeRequest ==
    /\ \E r \in reqs :
         /\ reqs' = reqs \ {r}
         /\ reps' = reps \cup
              { IF r.from >= pubNext - K
                  THEN [kind  |-> "data", first |-> r.from,
                        count |-> Min2(Min2(r.count, MaxBatch), pubNext - r.from)]
                  ELSE [kind |-> "nak", first |-> r.from, count |-> 0] }
    /\ UNCHANGED <<pubNext, mcast, lossLeft, dupLeft, expected, applied, buf, known, mode>>

DropReply ==
    /\ lossLeft > 0
    /\ \E r \in reps : reps' = reps \ {r}
    /\ lossLeft' = lossLeft - 1
    /\ UNCHANGED <<pubNext, mcast, reqs, dupLeft, expected, applied, buf, known, mode>>

\* Subscriber handles a retransmit reply.  A NAK for the current gap
\* switches to snapshot recovery; in the broken variant it is ignored and
\* the subscriber just keeps re-requesting.  Stale NAKs are ignored.
RecvReply ==
    /\ \E r \in reps :
         /\ reps' = reps \ {r}
         /\ IF r.kind = "data"
              THEN /\ Accept(r.first, r.count)
                   /\ UNCHANGED mode
              ELSE /\ mode' = IF EnableSnapshot /\ r.first = expected
                                THEN "snapshot" ELSE mode
                   /\ UNCHANGED <<expected, applied, buf>>
    /\ UNCHANGED <<pubNext, mcast, reqs, lossLeft, dupLeft, known>>

(* ---------------- Snapshot path ---------------- *)

\* Reliable TCP snapshot "as of S" with S = publisher's latest seq.  The
\* subscriber adopts the prefix up to S (appending ids expected..S, nothing
\* if multicast already moved it past S), discards buffered seqs <= S and
\* applies buffered seqs > S that are contiguous with S+1.
Snapshot ==
    /\ EnableSnapshot
    /\ mode = "snapshot"
    /\ LET S    == pubNext - 1
           e    == Max2(expected, S + 1)
           base == applied \o [i \in 1..(e - expected) |-> expected + i - 1]
       IN /\ Advance(base, e, {s \in buf : s > S})
          /\ known' = Max2(known, S)
    /\ mode' = "live"
    /\ UNCHANGED <<pubNext, mcast, reqs, reps, lossLeft, dupLeft>>

-----------------------------------------------------------------------------
Next ==
    \/ Publish \/ Heartbeat
    \/ RecvMcastAny \/ DupMcast \/ DropMcast
    \/ SendRequest \/ DropRequest \/ ServeRequest
    \/ DropReply \/ RecvReply
    \/ Snapshot

(***************************************************************************)
(* Fairness.  Weak fairness on every action of a *correct* party, none on  *)
(* the adversarial network actions (DropMcast, DupMcast, DropRequest,      *)
(* DropReply).  Loss and duplication are bounded by finite budgets, so     *)
(* "loss eventually ceases" holds in every behaviour.                      *)
(*   - Publish: the publisher eventually sends all N messages.            *)
(*   - Heartbeat: when traffic stops, heartbeats keep flowing, so a lost   *)
(*     tail message is eventually detected.                                *)
(*   - RecvMcastAny / RecvReply / ServeRequest: a packet that stays in     *)
(*     flight is eventually delivered.  Aggregate WF suffices because each *)
(*     non-duplicating delivery removes a packet, duplication is bounded   *)
(*     and heartbeats are only added when the channel is empty, so no      *)
(*     packet can be starved forever.                                      *)
(*   - SendRequest: the retransmit timeout eventually fires.               *)
(*   - Snapshot: the TCP snapshot eventually completes.                    *)
(* WF (not SF) is enough: each of these actions, once enabled, stays       *)
(* enabled until it is taken or its purpose is achieved by another step.   *)
(***************************************************************************)
Fairness ==
    /\ WF_vars(Publish)
    /\ WF_vars(Heartbeat)
    /\ WF_vars(RecvMcastAny)
    /\ WF_vars(SendRequest)
    /\ WF_vars(ServeRequest)
    /\ WF_vars(RecvReply)
    /\ WF_vars(Snapshot)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
(* Safety *)

TypeOK ==
    /\ pubNext  \in 1..(N + 1)
    /\ mcast    \subseteq Packet
    /\ reqs     \subseteq Request
    /\ reps     \subseteq Reply
    /\ lossLeft \in 0..MaxLoss
    /\ dupLeft  \in 0..MaxDup
    /\ expected \in 1..(N + 1)
    /\ applied  \in Seq(Seqs)
    /\ buf      \subseteq Seqs
    /\ known    \in 0..N
    /\ mode     \in {"live", "snapshot"}

\* Never applies a message twice.
NoDoubleApply ==
    \A i, j \in DOMAIN applied : i # j => applied[i] # applied[j]

\* Applies messages strictly in sequence order, with no holes.
InOrder ==
    \A i \in 1..(Len(applied) - 1) : applied[i + 1] = applied[i] + 1

\* The subscriber's state is always a prefix of the publisher's log.
PrefixOfPublisher ==
    /\ Len(applied) <= Len(PubLog)
    /\ applied = SubSeq(PubLog, 1, Len(applied))

\* `expected` is exactly one past what has been applied.
ExpectedConsistent == expected = Len(applied) + 1

\* Buffered messages are always strictly ahead of `expected`.
BufferAhead == \A s \in buf : s > expected

\* The subscriber never believes in a message that was not published.
KnownSound == known < pubNext

Safety ==
    /\ TypeOK /\ NoDoubleApply /\ InOrder /\ PrefixOfPublisher
    /\ ExpectedConsistent /\ BufferAhead /\ KnownSound

(* Liveness *)

FinalState == [i \in 1..N |-> i]

\* The subscriber eventually reaches the publisher's final state ...
EventuallyCaughtUp == <>(applied = FinalState)
\* ... and stays there (with safety this is the same, but check it).
CaughtUpForever    == <>[](applied = FinalState)

(* Non-vacuity probes: these are EXPECTED to be violated (see MC_Reach.cfg). *)

\* Violated iff the snapshot path is reachable.
NeverSnapshot == mode = "live"
\* Violated iff a NAK (gap older than the ring) is reachable.
NeverNak == \A r \in reps : r.kind = "data"

=============================================================================
