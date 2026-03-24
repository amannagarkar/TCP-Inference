/*
 * edge_collector.c
 *
 * TCP feature collector for Jetson edge device (WiFi client).
 * Two-thread design:
 *   Thread 1 — raw socket packet capture on wlan0
 *              extracts: 5-tuple, flags, seq, ack, payload,
 *                        passive RTT, inter-packet gap, retransmit flag
 *   Thread 2 — /proc/net/tcp poller (100ms interval)
 *              extracts: cwnd, rtt, rttvar, retrans, socket state,
 *                        snd_wnd, rcv_wnd, unacked, lost, sacked
 *
 * Both threads share a flow table (rwlock protected).
 * Output: rotating CSV files, one row per packet event.
 *
 * Build:
 *   gcc -O2 -Wall -std=c11 -o edge_collector edge_collector.c -lpthread -lm
 *
 * Run:
 *   sudo ./edge_collector -i wlan0 -o /capture/edge -r 60
 *
 * Note: /proc/net/tcp gives CWND only via /proc/net/tcp_info (linux 3.x+)
 *       or via ss(8) internals. We use the tcp_diag netlink socket for
 *       accurate cwnd — falls back to /proc/net/tcp for basic state.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_ether.h>

/* -------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------- */

#define DEFAULT_IFACE        "wlP1p1s0"
#define DEFAULT_OUTDIR       "/home/jetson-orin-2/Documents/TCP-Inference/client/logs"
#define DEFAULT_ROTATE_SECS  60
#define PROC_POLL_INTERVAL_MS 100       /* /proc poll rate               */
#define FLOW_TABLE_SIZE      1024       /* power of 2                    */
#define SEQ_TRACK_SIZE       64         /* in-flight seqs tracked/flow   */
#define FLOW_TIMEOUT_SEC     120
#define SNAPLEN              96

/* -------------------------------------------------------------------
 * TCP socket state names (/proc/net/tcp state column)
 * ------------------------------------------------------------------- */

static const char *TCP_STATES[] = {
    "UNKNOWN", "ESTABLISHED", "SYN_SENT", "SYN_RECV",
    "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT", "CLOSE",
    "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING"
};

#define TCP_STATE_NAME(s) \
    (((s) < (int)(sizeof(TCP_STATES)/sizeof(TCP_STATES[0]))) ? TCP_STATES[(s)] : "UNKNOWN")

/* -------------------------------------------------------------------
 * Shared flow table
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t seq_end;
    double   tx_ts;
    uint8_t  valid;
} SeqEntry;

typedef struct {
    /* Key (canonical: lower ip:port first) */
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t sport;
    uint16_t dport;
    uint8_t  valid;

    /* Packet-level state (written by capture thread) */
    double   last_pkt_ts;
    uint32_t last_seq;
    SeqEntry in_flight[SEQ_TRACK_SIZE];
    uint8_t  seq_head;
    time_t   last_seen;

    /* TCP socket state (written by poller thread) */
    uint32_t cwnd;              /* congestion window (segments)          */
    uint32_t snd_wnd;           /* send window (bytes)                   */
    uint32_t rcv_wnd;           /* receive window (bytes)                */
    uint32_t unacked;           /* bytes unacknowledged                  */
    uint32_t lost;              /* segments lost (kernel estimate)       */
    uint32_t sacked;            /* segments SACKed                       */
    uint32_t retrans;           /* total retransmissions                 */
    uint32_t rtt_us;            /* smoothed RTT from kernel (us)         */
    uint32_t rttvar_us;         /* RTT variance from kernel (us)         */
    uint32_t snd_ssthresh;      /* slow start threshold                  */
    uint8_t  tcp_state;         /* TCP state (ESTABLISHED etc.)          */
    uint8_t  ca_state;          /* congestion avoidance state            */
} FlowEntry;

static FlowEntry        flow_table[FLOW_TABLE_SIZE];
static pthread_rwlock_t flow_lock = PTHREAD_RWLOCK_INITIALIZER;

/* -------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------- */

static volatile int running      = 1;
static FILE        *csv_fp       = NULL;
static pthread_mutex_t csv_mutex = PTHREAD_MUTEX_INITIALIZER;
static char         outdir[512]  = DEFAULT_OUTDIR;
static char         iface[64]    = DEFAULT_IFACE;
static int          rotate_secs  = DEFAULT_ROTATE_SECS;
static time_t       file_open_ts = 0;

/* -------------------------------------------------------------------
 * Flow table helpers
 * ------------------------------------------------------------------- */

static uint32_t flow_hash(uint32_t sip, uint32_t dip,
                           uint16_t sp,  uint16_t dp)
{
    uint32_t h = 2166136261u;
    h ^= (sip ^ dip);   h *= 16777619u;
    h ^= ((uint32_t)sp ^ (uint32_t)dp); h *= 16777619u;
    return h & (FLOW_TABLE_SIZE - 1);
}

/* Canonical key: always store lower (ip,port) as src */
static void flow_canonicalize(uint32_t *sip, uint32_t *dip,
                               uint16_t *sp,  uint16_t *dp)
{
    if (*sip > *dip || (*sip == *dip && *sp > *dp)) {
        uint32_t t = *sip; *sip = *dip; *dip = t;
        uint16_t p = *sp;  *sp  = *dp;  *dp  = p;
    }
}

static FlowEntry *flow_get_locked(uint32_t sip, uint32_t dip,
                                   uint16_t sp,  uint16_t dp)
{
    /* caller holds write lock */
    flow_canonicalize(&sip, &dip, &sp, &dp);
    uint32_t idx = flow_hash(sip, dip, sp, dp);

    for (int i = 0; i < 16; i++) {
        FlowEntry *f = &flow_table[(idx + i) & (FLOW_TABLE_SIZE - 1)];
        if (!f->valid) {
            memset(f, 0, sizeof(*f));
            f->src_ip = sip; f->dst_ip = dip;
            f->sport  = sp;  f->dport  = dp;
            f->valid  = 1;
            f->last_seen = time(NULL);
            return f;
        }
        if (f->valid &&
            f->src_ip == sip && f->dst_ip == dip &&
            f->sport  == sp  && f->dport  == dp) {
            f->last_seen = time(NULL);
            return f;
        }
    }
    return NULL;
}

static void flow_evict_idle_locked(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < FLOW_TABLE_SIZE; i++)
        if (flow_table[i].valid &&
            (now - flow_table[i].last_seen) > FLOW_TIMEOUT_SEC)
            flow_table[i].valid = 0;
}

/* -------------------------------------------------------------------
 * CSV helpers
 * ------------------------------------------------------------------- */

static void csv_open_locked(void)
{
    char path[768];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    snprintf(path, sizeof(path),
             "%s/edge_features_%04d%02d%02d_%02d%02d%02d.csv",
             outdir,
             tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    csv_fp = fopen(path, "w");
    if (!csv_fp) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }

    fprintf(csv_fp,
        /* Packet-level */
        "ts,src_ip,dst_ip,sport,dport,"
        "direction,ip_len,payload_bytes,frame_len,"
        "dscp,ttl,tcp_flags,seq,ack,tcp_window,"
        "is_syn,is_ack,is_fin,is_rst,is_retrans,"
        "inter_pkt_gap_us,passive_rtt_us,"
        /* Socket-level (from kernel, may be 0 if not yet polled) */
        "cwnd,snd_wnd,rcv_wnd,unacked,lost,sacked,"
        "retrans,rtt_us,rttvar_us,snd_ssthresh,"
        "tcp_state,ca_state\n");

    fflush(csv_fp);
    file_open_ts = time(NULL);
}

static void csv_maybe_rotate(void)
{
    if (!csv_fp || (time(NULL) - file_open_ts) < rotate_secs)
        return;
    fclose(csv_fp);
    csv_fp = NULL;
    csv_open_locked();
    pthread_rwlock_wrlock(&flow_lock);
    flow_evict_idle_locked();
    pthread_rwlock_unlock(&flow_lock);
}

/* -------------------------------------------------------------------
 * Netlink / inet_diag helpers (for cwnd, rtt from kernel)
 *
 * We open a NETLINK_INET_DIAG socket and send a dump request for
 * all TCP sockets. The kernel returns struct inet_diag_msg + extension
 * INET_DIAG_INFO which contains struct tcp_info — same struct as
 * getsockopt(TCP_INFO) but accessible without owning the socket.
 * ------------------------------------------------------------------- */

static int nl_sock = -1;

static int nl_open(void)
{
    nl_sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_INET_DIAG);
    if (nl_sock < 0) {
        perror("netlink socket");
        return -1;
    }
    return 0;
}

typedef struct {
    struct nlmsghdr  nlh;
    struct inet_diag_req_v2 r;
} DiagReq;

static int nl_send_dump_request(void)
{
    DiagReq req = {0};
    req.nlh.nlmsg_len   = sizeof(req);
    req.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
    req.nlh.nlmsg_seq   = 1;
    req.r.sdiag_family   = AF_INET;
    req.r.sdiag_protocol = IPPROTO_TCP;
    req.r.idiag_states   = 0xFFF;       /* all states */
    req.r.idiag_ext     |= (1 << (INET_DIAG_INFO - 1));

    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    return sendto(nl_sock, &req, sizeof(req), 0,
                  (struct sockaddr *)&sa, sizeof(sa));
}

/*
 * Parse one netlink message, update flow table entry if found.
 * tcp_info is a large struct — we only read the fields we care about.
 */
static void nl_parse_msg(struct nlmsghdr *nlh)
{
    if (nlh->nlmsg_type == NLMSG_DONE || nlh->nlmsg_type == NLMSG_ERROR)
        return;

    struct inet_diag_msg *msg =
        (struct inet_diag_msg *)NLMSG_DATA(nlh);

    /* Only IPv4 TCP */
    if (msg->idiag_family != AF_INET)
        return;

    uint32_t sip = msg->id.idiag_src[0];
    uint32_t dip = msg->id.idiag_dst[0];
    uint16_t sp  = ntohs(msg->id.idiag_sport);
    uint16_t dp  = ntohs(msg->id.idiag_dport);

    /* Walk attributes to find INET_DIAG_INFO = struct tcp_info */
    struct rtattr *attr = (struct rtattr *)
        (((uint8_t *)msg) + NLMSG_ALIGN(sizeof(*msg)));
    int attr_len = nlh->nlmsg_len - NLMSG_SPACE(sizeof(*msg));

    struct tcp_info *ti = NULL;
    while (RTA_OK(attr, attr_len)) {
        if (attr->rta_type == INET_DIAG_INFO) {
            ti = (struct tcp_info *)RTA_DATA(attr);
            break;
        }
        attr = RTA_NEXT(attr, attr_len);
    }

    if (!ti)
        return;

    pthread_rwlock_wrlock(&flow_lock);
    FlowEntry *f = flow_get_locked(sip, dip, sp, dp);
    if (f) {
        f->cwnd        = ti->tcpi_snd_cwnd;
        f->snd_wnd     = 0;                        /* not in glibc tcp_info */
        f->rcv_wnd     = ti->tcpi_rcv_space;      /* rcv space         */
        f->unacked     = ti->tcpi_unacked;
        f->lost        = ti->tcpi_lost;
        f->sacked      = ti->tcpi_sacked;
        f->retrans     = ti->tcpi_retrans;
        f->rtt_us      = ti->tcpi_rtt;            /* smoothed RTT us   */
        f->rttvar_us   = ti->tcpi_rttvar;         /* RTT variance us   */
        f->snd_ssthresh = ti->tcpi_snd_ssthresh;
        f->tcp_state   = msg->idiag_state;
        f->ca_state    = ti->tcpi_ca_state;
    }
    pthread_rwlock_unlock(&flow_lock);
}

static void nl_recv_and_parse(void)
{
    char buf[65536];
    ssize_t n;

    while ((n = recv(nl_sock, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
        while (NLMSG_OK(nlh, (uint32_t)n)) {
            if (nlh->nlmsg_type == NLMSG_DONE)
                return;
            nl_parse_msg(nlh);
            nlh = NLMSG_NEXT(nlh, n);
        }
    }
}

/* -------------------------------------------------------------------
 * Poller thread — fires every PROC_POLL_INTERVAL_MS
 * ------------------------------------------------------------------- */

static void *poller_thread(void *arg)
{
    (void)arg;
    struct timespec ts = {
        .tv_sec  = 0,
        .tv_nsec = PROC_POLL_INTERVAL_MS * 1000000L
    };

    while (running) {
        nl_send_dump_request();
        nl_recv_and_parse();
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* -------------------------------------------------------------------
 * Capture thread — raw socket on wlan0
 * ------------------------------------------------------------------- */

static double ts_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static char *ip_str(uint32_t ip_net, char *buf)
{
    struct in_addr a = { .s_addr = ip_net };
    inet_ntop(AF_INET, &a, buf, INET_ADDRSTRLEN);
    return buf;
}

static void process_packet(const uint8_t *pkt, ssize_t pkt_len)
{
    /* Ethernet header */
    if (pkt_len < 14) return;
    uint16_t etype = (uint16_t)((pkt[12] << 8) | pkt[13]);
    if (etype != 0x0800) return;                /* IPv4 only */

    const struct ip *iph = (const struct ip *)(pkt + 14);
    if ((size_t)pkt_len < 14 + sizeof(struct ip)) return;
    if (iph->ip_p != IPPROTO_TCP) return;

    uint8_t  ip_hdr_len   = iph->ip_hl * 4;
    uint16_t ip_len       = ntohs(iph->ip_len);
    uint8_t  dscp         = iph->ip_tos >> 2;
    uint8_t  ttl          = iph->ip_ttl;
    uint32_t src_ip       = iph->ip_src.s_addr;
    uint32_t dst_ip       = iph->ip_dst.s_addr;

    const struct tcphdr *tcph =
        (const struct tcphdr *)((const uint8_t *)iph + ip_hdr_len);
    if ((size_t)pkt_len < 14 + ip_hdr_len + sizeof(struct tcphdr)) return;

    uint8_t  tcp_hdr_len  = tcph->th_off * 4;
    uint16_t sport        = ntohs(tcph->th_sport);
    uint16_t dport        = ntohs(tcph->th_dport);
    uint8_t  flags        = tcph->th_flags;
    uint32_t seq          = ntohl(tcph->th_seq);
    uint32_t ack_num      = ntohl(tcph->th_ack);
    uint16_t win          = ntohs(tcph->th_win);
    uint16_t payload_bytes = (ip_len > ip_hdr_len + tcp_hdr_len)
                             ? ip_len - ip_hdr_len - tcp_hdr_len : 0;

    int is_syn = (flags & TH_SYN) ? 1 : 0;
    int is_ack = (flags & TH_ACK) ? 1 : 0;
    int is_fin = (flags & TH_FIN) ? 1 : 0;
    int is_rst = (flags & TH_RST) ? 1 : 0;

    double ts = ts_now_us();

    /* Direction: 0 = this device is sender, 1 = this device is receiver.
     * We detect by checking which side has a local port in the flow.
     * Simpler heuristic: outbound if sport > 1024 and src_ip is local.
     * We store local IP at startup. */
    extern uint32_t local_ip_net;
    int direction = (src_ip == local_ip_net) ? 0 : 1;

    /* Flow table */
    pthread_rwlock_wrlock(&flow_lock);
    FlowEntry *f = flow_get_locked(src_ip, dst_ip, sport, dport);

    double gap_us        = 0.0;
    double passive_rtt   = -1.0;
    int    is_retrans    = 0;

    if (f) {
        if (f->last_pkt_ts > 0.0)
            gap_us = (ts - f->last_pkt_ts) * 1e6;
        f->last_pkt_ts = ts;

        if (payload_bytes > 0) {
            if (f->last_seq > 0 && seq <= f->last_seq)
                is_retrans = 1;
            else
                f->last_seq = seq;

            /* Record outbound segment for RTT estimation */
            if (direction == 0 && !is_retrans) {
                SeqEntry *s = &f->in_flight[f->seq_head % SEQ_TRACK_SIZE];
                s->seq_end  = seq + payload_bytes;
                s->tx_ts    = ts;
                s->valid    = 1;
                f->seq_head++;
            }
        }

        /* RTT: match inbound ACK to previously recorded outbound segment */
        if (is_ack && direction == 1) {
            for (int i = 0; i < SEQ_TRACK_SIZE; i++) {
                SeqEntry *s = &f->in_flight[i];
                if (s->valid && ack_num >= s->seq_end) {
                    passive_rtt = (ts - s->tx_ts) * 1e6;
                    s->valid = 0;
                    break;
                }
            }
        }

        /* Snapshot socket-level fields while holding lock */
        uint32_t cwnd       = f->cwnd;
        uint32_t snd_wnd    = f->snd_wnd;
        uint32_t rcv_wnd    = f->rcv_wnd;
        uint32_t unacked    = f->unacked;
        uint32_t lost       = f->lost;
        uint32_t sacked     = f->sacked;
        uint32_t retrans    = f->retrans;
        uint32_t rtt_us     = f->rtt_us;
        uint32_t rttvar_us  = f->rttvar_us;
        uint32_t ssthresh   = f->snd_ssthresh;
        uint8_t  tcp_state  = f->tcp_state;
        uint8_t  ca_state   = f->ca_state;

        pthread_rwlock_unlock(&flow_lock);

        /* Write CSV */
        char sip[INET_ADDRSTRLEN], dip[INET_ADDRSTRLEN];
        ip_str(src_ip, sip);
        ip_str(dst_ip, dip);

        pthread_mutex_lock(&csv_mutex);
        csv_maybe_rotate();
        fprintf(csv_fp,
            "%.6f,%s,%s,%u,%u,"     /* ts, 5-tuple              */
            "%d,%u,%u,%zd,"         /* dir, ip_len, payload, frame */
            "%u,%u,"                /* dscp, ttl                */
            "%u,%u,%u,%u,"          /* flags, seq, ack, win     */
            "%d,%d,%d,%d,%d,"       /* syn,ack,fin,rst,retrans  */
            "%.2f,%.2f,"            /* gap_us, passive_rtt_us   */
            "%u,%u,%u,%u,%u,%u,"    /* cwnd,sndwnd,rcvwnd,unack,lost,sack */
            "%u,%u,%u,%u,"          /* retrans,rtt,rttvar,ssthresh */
            "%s,%u\n",              /* tcp_state, ca_state      */
            ts, sip, dip, sport, dport,
            direction, ip_len, payload_bytes, pkt_len,
            dscp, ttl,
            flags, seq, ack_num, win,
            is_syn, is_ack, is_fin, is_rst, is_retrans,
            gap_us, passive_rtt,
            cwnd, snd_wnd, rcv_wnd, unacked, lost, sacked,
            retrans, rtt_us, rttvar_us, ssthresh,
            TCP_STATE_NAME(tcp_state), ca_state);
        pthread_mutex_unlock(&csv_mutex);

    } else {
        pthread_rwlock_unlock(&flow_lock);
    }
}

/* Local IP detection */
uint32_t local_ip_net = 0;

static void detect_local_ip(const char *ifname)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct ifreq ifr = {0};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
        local_ip_net = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
    close(fd);
    char buf[INET_ADDRSTRLEN];
    fprintf(stderr, "Local IP: %s\n", ip_str(local_ip_net, buf));
}

/* Reuse PCAP_BUFFER_MB from ap_collector convention */
#define PCAP_BUFFER_MB 64

static void *capture_thread(void *arg)
{
    (void)arg;
    uint8_t pkt[65536];

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("raw socket"); return NULL; }

    /* Bind to specific interface */
    struct ifreq ifr = {0};
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                   iface, strlen(iface)) < 0)
        perror("SO_BINDTODEVICE");

    /* Increase socket receive buffer */
    int rcvbuf = PCAP_BUFFER_MB * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    while (running) {
        ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
        if (n <= 0) continue;
        if (n > SNAPLEN) n = SNAPLEN;  /* only parse up to snaplen */
        process_packet(pkt, n);
    }

    close(fd);
    return NULL;
}

/* -------------------------------------------------------------------
 * Signal + main
 * ------------------------------------------------------------------- */

static void on_signal(int sig) { (void)sig; running = 0; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-i iface] [-o outdir] [-r rotate_secs]\n"
        "  -i  interface   default: wlan0\n"
        "  -o  output dir  default: /capture/edge\n"
        "  -r  rotate secs default: 60\n",
        prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "i:o:r:h")) != -1) {
        switch (opt) {
            case 'i': strncpy(iface,   optarg, sizeof(iface)-1);   break;
            case 'o': strncpy(outdir,  optarg, sizeof(outdir)-1);  break;
            case 'r': rotate_secs = atoi(optarg);                  break;
            default:  usage(argv[0]);
        }
    }

    mkdir(outdir, 0755);
    detect_local_ip(iface);

    if (nl_open() < 0) {
        fprintf(stderr, "Netlink unavailable — socket stats will be zero\n");
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    pthread_mutex_lock(&csv_mutex);
    csv_open_locked();
    pthread_mutex_unlock(&csv_mutex);

    fprintf(stderr, "Edge collector: iface=%s outdir=%s rotate=%ds\n",
            iface, outdir, rotate_secs);

    pthread_t cap_tid, poll_tid;
    pthread_create(&poll_tid, NULL, poller_thread,  NULL);
    pthread_create(&cap_tid,  NULL, capture_thread, NULL);

    pthread_join(cap_tid,  NULL);
    pthread_join(poll_tid, NULL);

    pthread_mutex_lock(&csv_mutex);
    if (csv_fp) { fflush(csv_fp); fclose(csv_fp); }
    pthread_mutex_unlock(&csv_mutex);

    if (nl_sock >= 0) close(nl_sock);
    fprintf(stderr, "Edge collector stopped.\n");
    return 0;
}
