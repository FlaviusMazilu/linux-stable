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

For NACK-isolation experiments, keep SACK enabled, set ``tcp_recovery=0``
and ``tcp_early_retrans=0`` independently, and raise both ``tcp_reordering``
and ``tcp_max_reordering`` above the maximum possible flight. RTO remains
enabled as a safety net. Do not blindly use INT_MAX: sequence-space tests
multiply the reordering threshold by MSS, so choose a bound whose product
is below 2**31. A threshold alone is not proof that no other recovery path
ran; verify the counters and retransmitted sequence ranges.

NACK-triggered recovery and the SACK scoreboard
--------------------------------------------

Entering Recovery because of a NACK must not force the SACK scoreboard's
``fast_rexmit`` head-loss shortcut. That shortcut assumes the oldest
unacknowledged skb was lost; a NACK may instead name a later segment.
With a reordered NACK, it could mark an earlier multi-segment skb lost even
with a reordering threshold of one million. The benchmark reproduced three
wire retransmissions from just one NACK loss mark, with no RTO or TLP.

The forced-head shortcut is therefore disabled for NACK-bearing ACKs.
Ordinary duplicate-ACK recovery is unchanged, and sufficient SACK evidence
still updates the scoreboard on such ACKs. RACK processing is also unchanged.
This is deliberately narrower than disabling the scoreboard on every
connection that negotiated trimming.
