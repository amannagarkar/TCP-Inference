/*
 * client.c — TCP client for WiFi power-save RTT experiment
 *
 * Sends length-prefixed messages to the server, records per-message RTT,
 * and optionally toggles 802.11 power-save on the WiFi interface via `iw`.
 *
 * Build:  gcc -O2 -o client client.c -lm
 * Run:
 *   ./client --server 192.168.1.100 --port 5001 \
 *            --msg-sizes 64,256,1024,4096 --num-messages 300 \
 *            --processing-delay 10 --power-save on --iface wlP1p1s0 \
 *            --output results/csv/run_ps_on.csv --run-id pd10_ps_on_gap0
 *		[Might need sudo privileges to manage ps mode]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/tcp.h>
#include <getopt.h>
#include <sys/stat.h>

/* ── limits ──────────────────────────────────────────────────────────────── */
#define MAX_SIZES        32
#define MAX_MESSAGES     100000
#define MAX_PAYLOAD      (1 << 20)
#define RESPONSE_LEN     2

/* ── timing ──────────────────────────────────────────────────────────────── */
static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void ms_sleep(double ms)
{
    if (ms <= 0.0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

/* ── socket helpers ──────────────────────────────────────────────────────── */
static int recv_exact(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, (char *)buf + done, n - done, 0);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int send_message(int fd, const uint8_t *payload, uint32_t plen)
{
    /* 4-byte big-endian length + payload in one writev-style send */
    uint32_t net_len = htonl(plen);
    uint8_t *buf = malloc(4 + plen);
    if (!buf) return -1;
    memcpy(buf, &net_len, 4);
    memcpy(buf + 4, payload, plen);
    ssize_t sent = send(fd, buf, 4 + plen, MSG_NOSIGNAL);
    free(buf);
    return (sent == (ssize_t)(4 + plen)) ? 0 : -1;
}

static int recv_response(int fd)
{
    uint32_t net_len;
    if (recv_exact(fd, &net_len, 4) < 0) return -1;
    uint32_t rlen = ntohl(net_len);
    if (rlen > 65536) return -1;
    uint8_t tmp[65536];
    return recv_exact(fd, tmp, rlen);
}

/* ── power-save control ──────────────────────────────────────────────────── */
static int set_power_save(const char *iface, int enable)
{
    const char *state = enable ? "on" : "off";
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (pid == 0) {
        /* child: exec iw */
        execlp("iw", "iw", "dev", iface, "set", "power_save", state, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
    if (!ok)
        fprintf(stderr, "[client] WARNING: iw power_save %s on %s failed (exit %d)\n",
                state, iface, WEXITSTATUS(status));
    else
        printf("[client] power_save %s on %s\n", state, iface);
    return ok;
}

/* ── parse comma-separated integers ─────────────────────────────────────── */
static int parse_sizes(const char *str, int *out, int max_out)
{
    int count = 0;
    char *buf = strdup(str);
    char *tok = strtok(buf, ",");
    while (tok && count < max_out) {
        out[count++] = atoi(tok);
        tok = strtok(NULL, ",");
    }
    free(buf);
    return count;
}

/* ── simple xorshift random (no stdlib rand dependency) ──────────────────── */
static uint32_t xstate = 0xdeadbeef;
static uint8_t xrand8(void) {
    xstate ^= xstate << 13;
    xstate ^= xstate >> 17;
    xstate ^= xstate << 5;
    return (uint8_t)(xstate & 0xff);
}

/* ── result record ───────────────────────────────────────────────────────── */
typedef struct {
    int      msg_index;
    int      msg_size;
    double   rtt_ms;
    double   t_send;
    double   t_recv;
} record_t;

/* ── usage ───────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -s, --server            <ip>     server IP (required)\n"
        "  -P, --port              <port>   server port          (default: 5001)\n"
        "  -S, --msg-sizes         <s,s,..> comma-separated payload sizes in bytes\n"
        "                                   (default: 64,256,1024,4096)\n"
        "  -n, --num-messages      <n>      messages per run     (default: 300)\n"
        "  -g, --inter-gap-ms      <ms>     gap between messages (default: 0)\n"
        "  -D, --processing-delay  <ms>     expected server delay, logged only\n"
        "  -p, --power-save        on|off   WiFi power save mode (default: off)\n"
        "  -i, --iface             <iface>  WiFi interface       (default: wlP1p1s0)\n"
        "  -o, --output            <file>   output CSV           (default: results/client_run.csv)\n"
        "  -r, --run-id            <id>     label for this run\n"
        "  -h, --help\n", prog);
    exit(1);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    /* defaults */
    const char *server_ip       = NULL;
    int         port            = 8000;
    const char *sizes_str       = "64,256,1024,4096";
    int         num_messages    = 300;
    double      inter_gap_ms    = 0.0;
    double      proc_delay_ms   = 10.0;
    int         power_save      = 0;
    const char *iface           = "wlP1p1s0";
    const char *output_path     = "results/client_run.csv";
    const char *run_id          = "";

    static struct option long_opts[] = {
        {"server",           required_argument, 0, 's'},
        {"port",             required_argument, 0, 'P'},
        {"msg-sizes",        required_argument, 0, 'S'},
        {"num-messages",     required_argument, 0, 'n'},
        {"inter-gap-ms",     required_argument, 0, 'g'},
        {"processing-delay", required_argument, 0, 'D'},
        {"power-save",       required_argument, 0, 'p'},
        {"iface",            required_argument, 0, 'i'},
        {"output",           required_argument, 0, 'o'},
        {"run-id",           required_argument, 0, 'r'},
        {"help",             no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c, idx;
    while ((c = getopt_long(argc, argv, "s:P:S:n:g:D:p:i:o:r:h", long_opts, &idx)) != -1) {
        switch (c) {
        case 's': server_ip     = optarg;                    break;
        case 'P': port          = atoi(optarg);              break;
        case 'S': sizes_str     = optarg;                    break;
        case 'n': num_messages  = atoi(optarg);              break;
        case 'g': inter_gap_ms  = atof(optarg);              break;
        case 'D': proc_delay_ms = atof(optarg);              break;
        case 'p': power_save    = (strcmp(optarg,"on")==0);  break;
        case 'i': iface         = optarg;                    break;
        case 'o': output_path   = optarg;                    break;
        case 'r': run_id        = optarg;                    break;
        default:  usage(argv[0]);
        }
    }
    if (!server_ip) { fprintf(stderr, "ERROR: --server required\n"); usage(argv[0]); }
    if (num_messages > MAX_MESSAGES) num_messages = MAX_MESSAGES;

    /* parse sizes */
    int sizes[MAX_SIZES];
    int n_sizes = parse_sizes(sizes_str, sizes, MAX_SIZES);
    if (n_sizes == 0) { fprintf(stderr, "ERROR: no valid --msg-sizes\n"); exit(1); }

    /* build size sequence: round-robin, then shuffle (Fisher-Yates) */
    int seq[MAX_MESSAGES];
    int per = num_messages / n_sizes;
    int fill = 0;
    for (int i = 0; i < n_sizes && fill < num_messages; i++)
        for (int j = 0; j < per && fill < num_messages; j++)
            seq[fill++] = sizes[i];
    /* fill remainder */
    for (int i = 0; fill < num_messages; i = (i+1) % n_sizes)
        seq[fill++] = sizes[i];
    /* Fisher-Yates shuffle */
    for (int i = num_messages - 1; i > 0; i--) {
        int j = (int)((uint32_t)xrand8() * (i + 1) / 256);
        int tmp = seq[i]; seq[i] = seq[j]; seq[j] = tmp;
    }

    /* make results dir */
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", output_path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    }

    /* set power save BEFORE connecting */
    int ps_ok = set_power_save(iface, power_save);
    ms_sleep(500); /* let driver apply PS state */

    /* connect */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip); exit(1);
    }
    printf("[client] connecting to %s:%d ...\n", server_ip, port);
    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect"); exit(1);
    }
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    printf("[client] connected — sending %d messages (PS=%s)\n",
           num_messages, power_save ? "on" : "off");

    /* allocate result buffer and payload scratch */
    record_t *results = calloc(num_messages, sizeof(record_t));
    uint8_t  *payload = malloc(MAX_PAYLOAD);
    if (!results || !payload) { fprintf(stderr, "OOM\n"); exit(1); }
    /* fill payload with random bytes once */
    for (int i = 0; i < MAX_PAYLOAD; i++) payload[i] = xrand8();

    /* ── message loop ── */
    int actual = 0;
    for (int i = 0; i < num_messages; i++) {
        int sz = seq[i];

        double t0 = mono_ms();
        double wall0 = wall_s();
        if (send_message(fd, payload, (uint32_t)sz) < 0) {
            fprintf(stderr, "[client] send failed at msg %d\n", i); break;
        }
        if (recv_response(fd) < 0) {
            fprintf(stderr, "[client] recv failed at msg %d\n", i); break;
        }
        double t1 = mono_ms();

        results[actual].msg_index = i;
        results[actual].msg_size  = sz;
        results[actual].rtt_ms    = t1 - t0;
        results[actual].t_send    = wall0;
        results[actual].t_recv    = wall0 + (t1 - t0) / 1000.0;
        actual++;

        if ((i + 1) % 50 == 0)
            printf("[client]  %d/%d  last RTT=%.2f ms\n", i+1, num_messages,
                   results[actual-1].rtt_ms);

        if (inter_gap_ms > 0.0) ms_sleep(inter_gap_ms);
    }

    /* signal end-of-session */
    uint32_t zero = 0;
    send(fd, &zero, 4, MSG_NOSIGNAL);
    close(fd);

    /* ── write CSV ── */
    FILE *fp = fopen(output_path, "w");
    if (!fp) { perror("fopen output"); exit(1); }
    fprintf(fp, "run_id,msg_index,msg_size_bytes,rtt_ms,t_send,t_recv,"
                "power_save,ps_set_ok,processing_delay_ms,iface,inter_gap_ms\n");
    for (int i = 0; i < actual; i++) {
        fprintf(fp, "%s,%d,%d,%.4f,%.9f,%.9f,%d,%d,%.3f,%s,%.3f\n",
            run_id,
            results[i].msg_index,
            results[i].msg_size,
            results[i].rtt_ms,
            results[i].t_send,
            results[i].t_recv,
            power_save,
            ps_ok,
            proc_delay_ms,
            iface,
            inter_gap_ms);
    }
    fclose(fp);

    /* ── summary stats ── */
    double sum = 0, mn = 1e9, mx = 0;
    for (int i = 0; i < actual; i++) {
        double r = results[i].rtt_ms;
        sum += r;
        if (r < mn) mn = r;
        if (r > mx) mx = r;
    }
    printf("[client] done  msgs=%d  RTT min=%.2f avg=%.2f max=%.2f ms\n",
           actual, mn, sum/actual, mx);
    printf("[client] results -> %s\n", output_path);

    free(results);
    free(payload);
    return 0;
}
