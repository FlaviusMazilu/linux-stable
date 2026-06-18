// SPDX-License-Identifier: GPL-2.0
/*
 * NACK-driven congestion control for trimming experiments.
 *
 * This congestion control reduces `snd_cwnd` by one MSS for every
 * `CA_ACK_NACK` notification. It also uses PRR for smooth decrease.
 * In a draft state, a lot of stub function used, does not tacke the
 * trimming_ok = false case;
 */

#include <linux/module.h>
#include <net/tcp.h>

struct nack_ca {
    u32 prior_cwnd;
    u32 loss_cwnd;
    u32 nack_count;
    u32 pkts_acked;   /* last observed pkts_acked sample */
    u32 nack_dec_accum; /* NACKs accumulated toward the next -1 reduction */
    bool nack_pending; /* CA_ACK_NACK seen in in_ack_event for this ACK */
};

static unsigned int nack_init_ssthresh __read_mostly = 0;
module_param(nack_init_ssthresh, uint, 0644);
MODULE_PARM_DESC(nack_init_ssthresh,
    "Initial ssthresh in segments for new connections (0 = kernel default)");

static unsigned int nack_cwnd_dec_factor __read_mostly = 1;
module_param(nack_cwnd_dec_factor, uint, 0644);
MODULE_PARM_DESC(nack_cwnd_dec_factor,
    "Soften the per-NACK cwnd reduction: reduce by 1/this_factor per NACK "
    "(apply -1 once every this_factor NACKs; default 1 = -1 per NACK)");


static void nack_init(struct sock *sk)
{
    struct nack_ca *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    BUILD_BUG_ON(sizeof(struct nack_ca) > ICSK_CA_PRIV_SIZE);

    ca->prior_cwnd = 0;
    ca->loss_cwnd = 0;
    ca->nack_count = 0;
    ca->pkts_acked = 0;
    ca->nack_dec_accum = 0;
    ca->nack_pending = false;

    if (nack_init_ssthresh)
        tp->snd_ssthresh = nack_init_ssthresh;
}

static u32 nack_ssthresh(struct sock *sk)
{
    const struct tcp_sock *tp = tcp_sk(sk);

    u32 ssthresh = tcp_snd_cwnd(tp); // prevent double -1 on recovery entry

    return ssthresh;
}

static void nack_in_ack_event(struct sock *sk, u32 flags)
{
    struct nack_ca *ca = inet_csk_ca(sk);

    if (flags & CA_ACK_NACK)
        ca->nack_pending = true; // just flag it here
}

static void nack_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
    struct nack_ca *ca = inet_csk_ca(sk);

    ca->pkts_acked = sample->pkts_acked;
}

static void nack_cwnd_event(struct sock *sk, enum tcp_ca_event ev)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct nack_ca *ca = inet_csk_ca(sk);

    switch (ev) {
    case CA_EVENT_LOSS:
        /* Record the cwnd at loss time for diagnostics/undo. */
        ca->loss_cwnd = tcp_snd_cwnd(tp);
        break;
    case CA_EVENT_TX_START:
        /* stub: can be used to rehash or update state at tx start */
        break;

    case CA_EVENT_COMPLETE_CWR:
        /* stub: can be used to update state upon completion of CWR phase */
        break;

    default:
        /* ignore other events for now */
        break;
    }
}

static u32 nack_undo_cwnd(struct sock *sk)
{
    /* No undo: return the current cwnd as the canonical value.
     * SACK is expected to be disabled for these experiments so DSACK
     * undo behavior is unnecessary.
     */
    struct tcp_sock *tp = tcp_sk(sk);

    return tcp_snd_cwnd(tp);
}

static void nack_set_state(struct sock *sk, u8 new_state)
{
    struct nack_ca *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u8 old_state = inet_csk(sk)->icsk_ca_state;

    /* set_state() is invoked by tcp_set_ca_state() *before* it stores
     * the new state, so `old_state` is the state we are leaving.
     */

    /* On entering Recovery record prior cwnd for possible undo. */
    if (new_state == TCP_CA_Recovery && old_state != TCP_CA_Recovery)
        ca->prior_cwnd = tcp_snd_cwnd(tp);

    /* Recovery/CWR exit: the core returns us to Open once high_seq is
     * acknowledged.  By this point the NACK-driven decrements have
     * converged on a sending rate that stopped producing loss, so
     * adopt the current cwnd as the new ssthresh -- this is the rate we
     * just proved to be safe.
     */
    if (new_state == TCP_CA_Open &&
        (old_state == TCP_CA_Recovery || old_state == TCP_CA_CWR)) {
        tp->snd_ssthresh = tcp_snd_cwnd(tp);
        tp->snd_cwnd_stamp = tcp_jiffies32;
    }

    /* Disable the core's cwnd-undo machinery for any cwnd-reduction
     * state.  Our NACK-driven decrements are deliberate reactions to
     * real trimming losses, not spurious reordering/RTO artifacts.  If
     * we leave tp->undo_marker set, an ACK that crosses high_seq makes
     * tcp_try_undo_recovery() treat the episode as spurious and revert
     * both snd_cwnd and snd_ssthresh (back to TCP_INFINITE_SSTHRESH),
     * wiping out every NACK reduction.  Clearing undo_marker here (it
     * was just set by tcp_init_undo() in tcp_enter_recovery/loss, which
     * runs before this set_state callback) keeps the CA authoritative
     * over cwnd and lets recovery exit cleanly to Open without undo.
     */
    if (new_state == TCP_CA_Recovery || new_state == TCP_CA_Loss)
        tp->undo_marker = 0;
}

static void nack_cong_control(struct sock *sk, u32 ack, int flag,
                              const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct nack_ca *ca = inet_csk_ca(sk);

    bool nack_event = ca->nack_pending;
    ca->nack_pending = false;

    if (nack_event) {
        u32 factor = max_t(u32, nack_cwnd_dec_factor, 1U);

        ca->nack_count++;

        /* Reduce the PRR target (snd_ssthresh) by 1 MSS for every
         * `factor` NACKs, i.e. by 1/factor per NACK on average.  The
         * accumulator carries the fractional remainder across ACKs so
         * the long-run reduction rate is exactly 1/factor.  factor == 1
         * reproduces the original "1 MSS per NACK" behaviour; a larger
         * factor softens the cwnd drop under heavy trimming bursts.
         */
        if (++ca->nack_dec_accum >= factor) {
            ca->nack_dec_accum = 0;
            tp->snd_ssthresh = max_t(u32, tp->snd_ssthresh - 1, 2U);
        }
    }

    if (tcp_in_cwnd_reduction(sk)) {
        /* CWR or Recovery: PRR owns the window.  newly_acked_sacked and
         * newly_lost come from the rate sample for this ACK.
         */
        tcp_cwnd_reduction(sk, rs ? rs->acked_sacked : 0,
                           rs ? rs->losses : 0, flag);
    } else if (rs && rs->acked_sacked) {
        /* Open/Disorder: standard Reno additive increase. */
        tcp_reno_cong_avoid(sk, ack, rs->acked_sacked);
    }

    tcp_update_pacing_rate(sk);
}

static size_t nack_get_info(struct sock *sk, u32 ext, int *attr, union tcp_cc_info *info)
{
    /* Stub: expose no extra fields yet. Return 0 bytes of cc info. */
    return 0;
}

static void nack_release(struct sock *sk)
{
    /* No dynamic allocations to free; stub present for symmetry. */
}

static struct tcp_congestion_ops tcp_nack __read_mostly = {
    .init        = nack_init,
    .release     = nack_release,
    .ssthresh    = nack_ssthresh,
    .cong_control = nack_cong_control,
    .cong_avoid  = tcp_reno_cong_avoid,
    .in_ack_event = nack_in_ack_event,
    .pkts_acked  = nack_pkts_acked,
    .cwnd_event  = nack_cwnd_event,
    .set_state   = nack_set_state,
    .undo_cwnd   = nack_undo_cwnd,
    .get_info    = nack_get_info,

    .owner = THIS_MODULE,
    .name  = "nack",
    .flags = TCP_CONG_NON_RESTRICTED,
};

static int __init tcp_nack_register(void)
{
    return tcp_register_congestion_control(&tcp_nack);
}

static void __exit tcp_nack_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_nack);
}

module_init(tcp_nack_register);
module_exit(tcp_nack_unregister);

MODULE_AUTHOR("Jimmy McGill");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NACK-driven congestion control: 1 MSS per NACK, Reno fallbacks");
