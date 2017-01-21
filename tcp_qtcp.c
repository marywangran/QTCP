/*
 * from fcicq's qreno，it is ok!
 * what is qtcp?it is terrible, but usable!
 */

#include <linux/module.h>
#include <net/tcp.h>

// from fcicq
#define QTCP_SHIFT 8
#define QTCP_UNIT (1<<8)
#define QTCP_HIGH_GAIN 739 // = 2 / ln2 * 256
#define QTCP_DRAIN_GAIN 61 // = ln2 * ln2 / 2 * 256, after LN2 cwnd
#define QTCP_NORMAL_GAIN 269 // = 1.05 * 256

static const int qtcp_gain[] = {QTCP_HIGH_GAIN, QTCP_DRAIN_GAIN, QTCP_NORMAL_GAIN, QTCP_UNIT};

enum qtcp_mode {
  QTCP_STARTUP = 0,
  QTCP_DRAIN = 1,
  QTCP_CONGAVOID = 2,
};

// from hstcp
static const struct qtcp_aimd_val {
  unsigned int cwnd;
  unsigned int md;
} qtcp_aimd_vals[] = {
  {     38,  128, /*  0.50 */ },
  {    118,  112, /*  0.44 */ },
  {    221,  104, /*  0.41 */ },
  {    347,   98, /*  0.38 */ },
  {    495,   93, /*  0.37 */ },
  {    663,   89, /*  0.35 */ },
  {    851,   86, /*  0.34 */ },
  {   1058,   83, /*  0.33 */ },
  {   1284,   81, /*  0.32 */ },
  {   1529,   78, /*  0.31 */ },
  {   1793,   76, /*  0.30 */ },
  {   2076,   74, /*  0.29 */ },
  {   2378,   72, /*  0.28 */ },
  {   2699,   71, /*  0.28 */ },
  {   3039,   69, /*  0.27 */ },
  {   3399,   68, /*  0.27 */ },
  {   3778,   66, /*  0.26 */ },
  {   4177,   65, /*  0.26 */ },
  {   4596,   64, /*  0.25 */ },
  {   5036,   62, /*  0.25 */ },
  {   5497,   61, /*  0.24 */ },
  {   5979,   60, /*  0.24 */ },
  {   6483,   59, /*  0.23 */ },
  {   7009,   58, /*  0.23 */ },
  {   7558,   57, /*  0.22 */ },
  {   8130,   56, /*  0.22 */ },
  {   8726,   55, /*  0.22 */ },
  {   9346,   54, /*  0.21 */ },
  {   9991,   53, /*  0.21 */ },
  {  10661,   52, /*  0.21 */ },
  {  11358,   52, /*  0.20 */ },
  {  12082,   51, /*  0.20 */ },
  {  12834,   50, /*  0.20 */ },
  {  13614,   49, /*  0.19 */ },
  {  14424,   48, /*  0.19 */ },
  {  15265,   48, /*  0.19 */ },
  {  16137,   47, /*  0.19 */ },
  {  17042,   46, /*  0.18 */ },
  {  17981,   45, /*  0.18 */ },
  {  18955,   45, /*  0.18 */ },
  {  19965,   44, /*  0.17 */ },
  {  21013,   43, /*  0.17 */ },
  {  22101,   43, /*  0.17 */ },
  {  23230,   42, /*  0.17 */ },
  {  24402,   41, /*  0.16 */ },
  {  25618,   41, /*  0.16 */ },
  {  26881,   40, /*  0.16 */ },
  {  28193,   39, /*  0.16 */ },
  {  29557,   39, /*  0.15 */ },
  {  30975,   38, /*  0.15 */ },
  {  32450,   38, /*  0.15 */ },
  {  33986,   37, /*  0.15 */ },
  {  35586,   36, /*  0.14 */ },
  {  37253,   36, /*  0.14 */ },
  {  38992,   35, /*  0.14 */ },
  {  40808,   35, /*  0.14 */ },
  {  42707,   34, /*  0.13 */ },
  {  44694,   33, /*  0.13 */ },
  {  46776,   33, /*  0.13 */ },
  {  48961,   32, /*  0.13 */ },
  {  51258,   32, /*  0.13 */ },
  {  53677,   31, /*  0.12 */ },
  {  56230,   30, /*  0.12 */ },
  {  58932,   30, /*  0.12 */ },
  {  61799,   29, /*  0.12 */ },
  {  64851,   28, /*  0.11 */ },
  {  68113,   28, /*  0.11 */ },
  {  71617,   27, /*  0.11 */ },
  {  75401,   26, /*  0.10 */ },
  {  79517,   26, /*  0.10 */ },
  {  84035,   25, /*  0.10 */ },
  {  89053,   24, /*  0.10 */ },
  // TODO ????
};

#define HSTCP_AIMD_MAX	ARRAY_SIZE(qtcp_aimd_vals)

struct qtcp {
  u32 prior_cwnd;
  u32 pipe_cwnd;
  u32 drain_cwnd;
  u32 ai;
  u8 prev_ca_state; // 3 bits would be enough
  u8 mode;
  u8 loss_round;
};

static void tcp_qtcp_init(struct sock *sk)
{
  const struct tcp_sock *tp = tcp_sk(sk);
  struct qtcp *ca = inet_csk_ca(sk);

  u64 rate;
  rate = (u64)tp->mss_cache * 2 * (USEC_PER_SEC << 3); 
  rate *= max(tp->snd_cwnd, tp->packets_out);
  if (tp->srtt_us)
    do_div(rate, tp->srtt_us);
  sk->sk_pacing_rate = min_t(u64, rate, sk->sk_max_pacing_rate);

  ca->prior_cwnd = 0;
  ca->pipe_cwnd = 0;
  ca->drain_cwnd = 0;
  ca->prev_ca_state = TCP_CA_Open;
  ca->mode = QTCP_STARTUP;
  ca->loss_round = 0;
  ca->ai = 0;
}

// from fcicq and fucking bbr
static void tcp_qtcp_save_cwnd(struct sock *sk)
{
  const struct tcp_sock *tp = tcp_sk(sk);
  struct qtcp *ca = inet_csk_ca(sk);
  if (ca->prev_ca_state < TCP_CA_Recovery)
    ca->prior_cwnd = tp->snd_cwnd;
  else
    ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
  if (ca->pipe_cwnd == 0)
    ca->pipe_cwnd = ca->prior_cwnd;
}

static u32 tcp_qtcp_ssthresh(struct sock *sk)
{
  tcp_qtcp_save_cwnd(sk);
  return TCP_INFINITE_SSTHRESH;
}

static u32 tcp_qtcp_cwnd_undo(struct sock *sk)
{
  const struct qtcp *ca = inet_csk_ca(sk);
  return max(tcp_sk(sk)->snd_cwnd, ca->prior_cwnd);
}

static void qtcp_update_pacing(struct sock *sk, int gain, u32 cwnd, u32 rtt)
{
  struct tcp_sock *tp = tcp_sk(sk);
  u64 rate;

  rate = (u64)tp->mss_cache;
  rate *= ((u64) cwnd * gain) + QTCP_UNIT - 1;
    
  rate *= (USEC_PER_SEC << 3);
  rate >>= QTCP_SHIFT;
  if (likely(rtt))
    do_div(rate, rtt);
  
  ACCESS_ONCE(sk->sk_pacing_rate) = min_t(u64, rate,
					sk->sk_max_pacing_rate);
}

static void update_qtcp_ai(struct sock *sk)
{
  struct tcp_sock *tp = tcp_sk(sk);
  struct qtcp *ca = inet_csk_ca(sk);
  if (tp->snd_cwnd > qtcp_aimd_vals[ca->ai].cwnd) {
    while (tp->snd_cwnd > qtcp_aimd_vals[ca->ai].cwnd && ca->ai < HSTCP_AIMD_MAX - 1)
      ca->ai++;
  } else if (ca->ai && tp->snd_cwnd <= qtcp_aimd_vals[ca->ai-1].cwnd) {
    while (ca->ai && tp->snd_cwnd <= qtcp_aimd_vals[ca->ai-1].cwnd)
      ca->ai--;
  }
}

static void tcp_qtcp_main(struct sock *sk, const struct rate_sample *rs)
{
  struct tcp_sock *tp = tcp_sk(sk);
  struct qtcp *ca = inet_csk_ca(sk);
  u32 acked = rs->acked_sacked;
  u32 cwnd = tp->snd_cwnd, pacing_cwnd;
  u8 prev_state = ca->prev_ca_state, state = inet_csk(sk)->icsk_ca_state;

  tcp_qtcp_save_cwnd(sk);
  ca->prev_ca_state = state; 

  if (rs->losses > 0) {
    if (ca->loss_round == 1) {
      update_qtcp_ai(sk);
      tp->snd_cwnd = max(tp->snd_cwnd - ((tp->snd_cwnd * qtcp_aimd_vals[ca->ai].md) >> 8), 4U);
      ca->pipe_cwnd = tp->snd_cwnd;
      ca->loss_round = 0;
    }
    tp->snd_cwnd = cwnd = max_t(s32, cwnd - rs->losses, ca->pipe_cwnd);
  } else {
      ca->loss_round = 1;
  }

  if (ca->mode == QTCP_STARTUP) {
    if (rs->losses == 0) {
      cwnd = tp->snd_cwnd + acked;
    } else {
      cwnd = ca->drain_cwnd = max(ca->pipe_cwnd, ca->prior_cwnd);
      cwnd = min(cwnd, tp->snd_cwnd + acked);
      ca->mode = QTCP_DRAIN; 
    }
    tp->snd_cwnd = max(min(cwnd, tp->snd_cwnd_clamp), 2U);
  } else if (ca->mode == QTCP_DRAIN) {
    if (tcp_packets_in_flight(tp) <= ca->drain_cwnd) {
      ca->mode = QTCP_CONGAVOID;
    }
  } else if (ca->mode == QTCP_CONGAVOID) { 
    if (state >= TCP_CA_Recovery) {
      if (prev_state != TCP_CA_Recovery)
        cwnd = tcp_packets_in_flight(tp) + acked;
      tp->snd_cwnd = max(cwnd, tcp_packets_in_flight(tp) + acked);
    } else { 
      if (prev_state >= TCP_CA_Recovery) {
        tp->snd_cwnd = max(cwnd, ca->prior_cwnd); 
        ca->prior_cwnd = tp->snd_cwnd;
      }
      update_qtcp_ai(sk);

      if (tp->snd_cwnd < tp->snd_cwnd_clamp) {
        tp->snd_cwnd_cnt += ca->ai + 1;
        if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
          tp->snd_cwnd_cnt -= tp->snd_cwnd;
          tp->snd_cwnd++;
        }
      }
    }
  }
  
  pacing_cwnd = ca->prior_cwnd;
  if (state >= TCP_CA_Recovery)
    pacing_cwnd = ca->pipe_cwnd;
// I want to use rs->rtt_us or bbr's filtered bbr->min_rtt_us here,but ...
  qtcp_update_pacing(sk, qtcp_gain[ca->mode], pacing_cwnd, tp->srtt_us);
}

static void tcp_qtcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
  if (event == CA_EVENT_CWND_RESTART)
    tcp_qtcp_init(sk);
}

static void qtcp_set_state(struct sock *sk, u8 new_state)
{
  struct qtcp *ca = inet_csk_ca(sk);

  if (new_state == TCP_CA_Loss) {
    u32 tmp_cwnd = ca->pipe_cwnd;
    tcp_qtcp_init(sk);
    ca->pipe_cwnd = tmp_cwnd;
    ca->prev_ca_state = TCP_CA_Loss;
  }
}

static struct tcp_congestion_ops tcp_qtcp __read_mostly = {
  .init = tcp_qtcp_init,
  .ssthresh = tcp_qtcp_ssthresh,
  .undo_cwnd = tcp_qtcp_cwnd_undo,
  .cong_control = tcp_qtcp_main,
  .cwnd_event = tcp_qtcp_cwnd_event,
  .set_state = qtcp_set_state,
  .owner = THIS_MODULE,
  .name = "qtcp",
};

static int __init tcp_qtcp_register(void)
{
  return tcp_register_congestion_control(&tcp_qtcp);
}

static void __exit tcp_qtcp_unregister(void)
{
  tcp_unregister_congestion_control(&tcp_qtcp);
}

module_init(tcp_qtcp_register);
module_exit(tcp_qtcp_unregister);

MODULE_AUTHOR("mich & fcicq & marywangran");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QReno TCP");
