Packet trimming measurement counters
====================================

The experimental packet-trimming implementation exports the following
per-network-namespace TcpExt counters through ``/proc/net/netstat``:

* ``TCPTrimmingNacksSent``: NACK options constructed for transmission.
  This is not proof of delivery; compare it with a packet capture.
* ``TCPTrimmingNacksReceived``: valid, negotiated NACK options parsed.
  A parsed NACK may subsequently be rejected by ACK/window validation.
* ``TCPTrimmingLossMarks``: wire segments newly marked lost by the NACK
  handler, including retransmissions declared lost again. This does not
  count loss marks made by RACK, the SACK scoreboard, or the RTO handler.
* ``TCPTrimmingNacksUnproductive``: calls to the NACK loss handler that
  made no new loss mark, for example an already acknowledged segment or
  a retransmission protected by the min-RTT guard. NACKs rejected before
  reaching the loss handler are not included.

Take before/after snapshots in an otherwise idle namespace. Do not infer
causality merely from a NACK preceding a retransmission, particularly when
RACK is enabled. Pair these counters with ``TCPTimeouts``, ``TCPLossProbes``,
``TCPFastRetrans``, TCP_INFO, and sender/receiver captures.

Packet-path and handshake diagnostics use debug logging so console I/O does
not become part of the measured trimming cost. The counters themselves do
not alter loss detection or congestion-control policy.

Recovery controls on the updated kernel
--------------------------------------

Linux 7.3.3 no longer disables RACK when ``tcp_recovery=0``. Upstream
removed the RFC 3517/6675 SACK loss detector in commit 1c120191dcec.
This branch deliberately preserves that upstream behavior; it does not
restore the old detector or add an experimental RACK-disable override.

Consequently, the old benchmark labels ``trim_only``, ``dupack``, and
``rto`` are not valid detector-isolation configurations on this kernel.
The existing ``rack`` and ``trim_rack`` configurations remain appropriate
for comparing RACK-TLP with and without trimming. ``tcp_early_retrans=0``
still disables TLP, but does not disable RACK. SACK remains enabled.

The preserved 6.14-rc4 benchmark kernel at c9d705d78d97 supports the old
NACK-isolation configuration: ``tcp_recovery=0``, ``tcp_early_retrans=0``,
and both reordering thresholds raised above the maximum possible flight.
Do not silently apply that interpretation to results from this branch.
RTO remains enabled as a safety net.

Upstream also bounds ``tcp_reordering`` by ``tcp_max_reordering``.
When raising both, set the maximum first. The old harness writes these
in the opposite order, so its large-threshold modes need adaptation even
before addressing their changed recovery semantics. Do not use INT_MAX.

NACK-triggered recovery and the SACK scoreboard
----------------------------------------------

Entering Recovery because of a NACK must not force the SACK scoreboard's
``fast_rexmit`` head-loss shortcut. That shortcut assumes the oldest
unacknowledged skb was lost; a NACK may instead name a later segment.
With a reordered NACK, it could mark an earlier multi-segment skb lost even
with a reordering threshold of one million. The benchmark reproduced three
wire retransmissions from just one NACK loss mark, with no RTO or TLP.

The tested 6.14-rc4 kernel disables the forced-head shortcut only for
NACK-bearing ACKs, in commit c9d705d78d97. On this updated branch, upstream
has removed the entire legacy scoreboard loss-marking path, including
that shortcut. The fix is preserved in history and its purpose is satisfied
by the path's removal; the deleted code is not resurrected during merging.
SACK receipt tracking and RACK processing still operate normally.

Upstream integration: Linux 7.3.3
--------------------------------

The integration branch is ``experiment/upstream-sync-20261107``. It starts
from the exact tested kernel, c9d705d78d97f9cd8eea4a0cb387e6cd5cc71ee8,
including the experiment counters from ea558fcb6f65. The common upstream
base is dd83757f6e686a2188997cb58b5975f744bb7786 (6.14-rc4).

On 2026-11-07, kernel.org listed 7.3.3 as the latest stable release.
The selected release tag is ``v7.3.3``:

* Annotated tag: b7cb9f473aa648c37e68d08cb2f3c154f17f593c.
* Release commit: d50d994bffa3c7c6db2d8bc4c5f0c55e3f46b0cd.

Both object IDs were checked against the official stable repository,
independently of the configured ``gregkh/linux`` GitHub mirror. The merge
preserves both histories. The original experiment branch and its existing
benchmark build artifacts are not replaced.

Conflict resolutions
~~~~~~~~~~~~~~~~~~~~

Git reported conflicts in seven files:

* ``include/linux/tcp.h``: retain AccECN state and trimming state in
  distinct bitfields, including the newly occupied SYN ECN bits.
* ``include/net/tcp.h``: retain both option families and includes; adjust
  the skb control-block bitfield padding for the enlarged timestamp flags.
* ``net/ipv4/proc.c``: retain all four trimming counters while adopting
  upstream's removal of the SNMP array sentinel.
* ``net/ipv4/tcp.c``: retain upstream's cacheline member checks rather
  than restoring the removed old group-size assertion.
* ``net/ipv4/tcp_input.c``: move ``FLAG_NACK`` to an unused bit because
  upstream now uses its former bit for ``FLAG_TS_PROGRESS``. Preserve NACK
  loss marking and counters, and map ``CA_ACK_NACK`` through upstream's
  relocated congestion-control ACK callback. Accept the removal of the
  legacy scoreboard and preserve the trimmed SYN-RECV handling.
* ``net/ipv4/tcp_ipv4.c``: retain both required includes.
* ``net/ipv4/tcp_output.c``: allocate separate internal flags for AccECN,
  trimming negotiation, and NACKs, and retain both required includes.

An additional semantic adaptation is necessary even though Git did not
flag its location: emit trimming options after upstream has written any
pending AccECN continuation bytes and SACK alignment. Otherwise a trimming
option could interrupt a partially emitted AccECN option. Wire option
numbers 42 and 43, DSCP values, NACK-directed MSS selection, and the min-RTT
retransmission guard are unchanged. No NACK-as-delivery or PRR-policy change
is introduced.

Changes relevant to these experiments
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The following are upstream changes since the common base, not claims of
measured performance improvements:

* **Loss-detector selection:** 1c120191dcec removes the legacy SACK loss
  detector and makes ``tcp_recovery=0`` ineffective as a RACK-disable
  switch. This is the most important compatibility change for the study.
* **SACK compression:** 416dd649f3aa adds
  ``tcp_comp_sack_rtt_percent`` and changes the default from the old fixed
  5% to 33%. The nominal delay remains capped at 1 ms; timer slack is now
  10 us instead of 100 us (ca412f25d6b2). At a 10 ms RTT, the nominal cap changes the
  example delay from 0.5 ms to 1 ms. This can affect feedback timing in
  short flows; it is not an ACK-ratio protocol or RACK-only feature.
* **Direct recovery instrumentation:** 8e677a466145 adds the raw tracepoint
  ``tcp_cwnd_reduction_tp``. This provides a hook for observing the
  congestion-window reduction path rather than inferring every internal
  PRR decision from captures. It is not a regular tracefs event, and it is
  emitted after the zero-delivery early return: pure NACKs need not produce
  an event.
* **RTO controls:** 54a378f43425, 1280c26228bd, and f38805c5d26f add
  maximum-RTO controls and the ``TCP_RTO_MIN_US`` socket option. Current
  documentation describes route/socket/sysctl precedence; defaults remain
  200 ms minimum and 120 s maximum. The existing route-based minimum-RTO
  experiments should continue to record the effective settings.
* **Accurate ECN and TCP options:** 542a495cbaa6, 3cae34274c79,
  b5e74132dfbe, and e7e9da850a46 add AccECN negotiation, counters, options,
  and option-space sharing with SACK. 149dfb31615e relocates ACK event
  delivery to congestion control. These changes directly affect the
  trimming port even though the original performance studies set ECN to 0.
* **Reordering controls:** efb8763d7bbb validates positive thresholds and
  bounds initial reordering by the per-namespace maximum. Setup order now
  matters; large values are not a way to disable time-based RACK.
* **Receive-buffer behavior:** 63ad7dfedfae and aa251c84636c adjust receive
  autotuning, including behavior under reordering; ecfea98b7d0d adds
  ``tcp_rcvbuf_low_rtt`` (default 1 ms). These are relevant to bulk and
  reordered-flow studies, not proof of a change in detector quality.
* **Virtual-link backpressure:** dc82a33297fc changes veth's full-ring
  behavior with NAPI and an attached qdisc from immediate drop to queue
  backpressure. This can affect unintended losses and queueing in the VM
  topology when those conditions apply; it is not a change to the custom
  trimming qdisc's selected loss pattern.
* **Netem reproducibility and accounting:** 4185701fcce6 includes reordered
  packets in queue-limit checks; 986afaf80994 avoids reseeding on changes
  without an explicit seed; a2f6ed7b4873 adds impairment counters. The
  benchmark's explicit seeds and endpoint captures remain important.
* **TCP scheduling:** fd0406e5ca53 converts TCP Small Queues from tasklets
  to BH workqueues. Upstream intends equivalent behavior, not a new loss
  detector; scheduling overhead is a potential cross-version confounder.

Old and new kernel results must remain separate. An integration smoke run
does not establish that the earlier performance conclusions still hold on
7.3.3, and does not recreate a valid trimming-only configuration.

Integration validation
~~~~~~~~~~~~~~~~~~~~~~

The code merge is 822c3f28c1809cd8ed55f612ccff003f3969eb17. An isolated
detached worktree was built from that commit with the previously tested
configuration migrated using ``make olddefconfig`` and a distinct
``-trimbench-upstream`` local version. ``bzImage`` and modules built with
GCC 13.3.0, and the unchanged ``sch_trimbench`` source built against the new
kernel. The booted release was
``7.3.3-trimbench-upstream-00021-g822c3f28c180``.

Three one-vCPU KVM integration suites completed without invalid trials or
reported kernel errors:

* 18 ordinary cases: nine scenarios in ``rack`` and ``trim_rack``, including
  head/middle/tail losses, three-packet bursts, reordered NACKs, and silently
  dropped tails.
* Eight forward-reordering cases: head and middle-burst faults with
  reordering off/on in both supported modes.
* Four AccECN cases: clean and first-segment loss in both modes, with
  ``tcp_ecn=3`` in an isolated harness snapshot. Captures contain valid
  AccECN options and a packet carrying both AccECN and a trimming NACK.

All 68 pcaps were independently reread. The standard capture checks verify
negotiation, exact trim/NACK correspondence, sequence numbers, header-only
CS6 NACKs, and preserved checksums. Runtime sysctl snapshots confirm the
new SACK compression defaults and unchanged default RTO bounds.

Artifacts are kept separately under the infrastructure repository's
``bench/artifacts/upstream-7.3.3/`` directory. It contains the pinned build
worktree, configuration delta, isolated harness and build manifest, and
``smoke-default``, ``smoke-forward``, and ``smoke-accecn`` results with source
snapshots. The original build manifest, image, config, qdisc module, and
initramfs were hash-checked and remain unchanged.

``git diff --check`` passes for the custom delta against v7.3.3. The strict
checkpatch run still reports inherited trimming style issues: two errors,
11 warnings, and 11 checks, versus two errors, 12 warnings, and 11 checks
for the same file set in the old fork. Those pre-existing issues were not
silently treated as a clean lint result or expanded into an unrelated
formatting cleanup.
