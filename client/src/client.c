/*
 * client.c — TCP client for WiFi power-save RTT experiment
 *
 * Splits a packet into NUM_SEGMENTS segments and sends each as:
 *   [seq_num (4B BE)] [payload_size (4B BE)] [payload]
 * Then waits for server to reply with "OK" + last sequence number.
 *
 * Build:  gcc -O2 -o client client.c -lm
 * Run:
 *   ./client --server 192.168.1.100 --port 5001 \
 *            --seg-size 64 --power-save on --iface wlP1p1s0 \
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

/* ── segment protocol ────────────────────────────────────────────────────── */
#define NUM_SEGMENTS  1000          /* total segments to send per transfer */
#define MAX_SEG_SIZE  (1 << 16)    /* max payload per segment (64 KiB) */

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

/* send one segment: [seq(4B BE)][size(4B BE)][payload] */
static int send_segment(int fd, uint32_t seq, const uint8_t *payload, uint32_t size)
{
    uint8_t hdr[8];
    uint32_t net_seq  = htonl(seq);
    uint32_t net_size = htonl(size);
    memcpy(hdr,     &net_seq,  4);
    memcpy(hdr + 4, &net_size, 4);
    if (send(fd, hdr, 8, MSG_NOSIGNAL) != 8) return -1;
    ssize_t sent = send(fd, payload, size, MSG_NOSIGNAL);
    return (sent == (ssize_t)size) ? 0 : -1;
}

/* recv server OK response: [4B len prefix]["OK"][4B last_seq BE]
   response payload is 6 bytes: 'O','K' + uint32 last_seq              */
static int recv_ok_response(int fd, uint32_t *out_last_seq)
{
    uint32_t net_len;
    if (recv_exact(fd, &net_len, 4) < 0) return -1;
    uint32_t rlen = ntohl(net_len);
    if (rlen != 6) { fprintf(stderr, "[client] unexpected response len %u\n", rlen); return -1; }
    uint8_t buf[6];
    if (recv_exact(fd, buf, 6) < 0) return -1;
    if (buf[0] != 'O' || buf[1] != 'K') {
        fprintf(stderr, "[client] expected OK, got %c%c\n", buf[0], buf[1]); return -1;
    }
    uint32_t net_seq;
    memcpy(&net_seq, buf + 2, 4);
    *out_last_seq = ntohl(net_seq);
    return 0;
}

/* ── power-save control ──────────────────────────────────────────────────── */
static int set_power_save(const char *iface, int enable)
{
    const char *state = enable ? "on" : "off";
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (pid == 0) {
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

/* ── simple xorshift random ───────────────────────────────────────────────── */
static uint32_t xstate = 0xdeadbeef;
static uint8_t xrand8(void) {
    xstate ^= xstate << 13;
    xstate ^= xstate >> 17;
    xstate ^= xstate << 5;
    return (uint8_t)(xstate & 0xff);
}

/* ── usage ───────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -s, --server       <ip>    server IP (required)\n"
        "  -P, --port         <port>  server port         (default: 5001)\n"
        "  -z, --seg-size     <bytes> payload per segment (default: 64)\n"
        "  -p, --power-save   on|off  WiFi power save     (default: off)\n"
        "  -i, --iface        <iface> WiFi interface      (default: wlP1p1s0)\n"
        "  -o, --output       <file>  output CSV          (default: results/client_run.csv)\n"
        "  -r, --run-id       <id>    label for this run\n"
        "  -h, --help\n"
        "\nSends %d segments of [seq, size, payload] then awaits OK + last_seq.\n",
        prog, NUM_SEGMENTS);
    exit(1);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    const char *server_ip  = NULL;
    int         port       = 5001;
    int         seg_size   = 64;
    int         power_save = 0;
    const char *iface      = "wlP1p1s0";
    const char *output_path = "results/client_run.csv";
    const char *run_id     = "";

    static struct option long_opts[] = {
        {"server",     required_argument, 0, 's'},
        {"port",       required_argument, 0, 'P'},
        {"seg-size",   required_argument, 0, 'z'},
        {"power-save", required_argument, 0, 'p'},
        {"iface",      required_argument, 0, 'i'},
        {"output",     required_argument, 0, 'o'},
        {"run-id",     required_argument, 0, 'r'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c, idx;
    while ((c = getopt_long(argc, argv, "s:P:z:p:i:o:r:h", long_opts, &idx)) != -1) {
        switch (c) {
        case 's': server_ip  = optarg;                   break;
        case 'P': port       = atoi(optarg);             break;
        case 'z': seg_size   = atoi(optarg);             break;
        case 'p': power_save = (strcmp(optarg,"on")==0); break;
        case 'i': iface      = optarg;                   break;
        case 'o': output_path = optarg;                  break;
        case 'r': run_id     = optarg;                   break;
        default:  usage(argv[0]);
        }
    }
    if (!server_ip) { fprintf(stderr, "ERROR: --server required\n"); usage(argv[0]); }
    if (seg_size < 1 || seg_size > MAX_SEG_SIZE) {
        fprintf(stderr, "ERROR: --seg-size must be 1..%d\n", MAX_SEG_SIZE); exit(1);
    }

    /* make results dir */
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", output_path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    }

    int ps_ok = set_power_save(iface, power_save);
    ms_sleep(500);

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
    printf("[client] connected — sending %d segments of %d bytes each (PS=%s)\n",
           NUM_SEGMENTS, seg_size, power_save ? "on" : "off");

    /* fill segment payload with random bytes */
    uint8_t *payload = malloc((size_t)seg_size);
    if (!payload) { fprintf(stderr, "OOM\n"); exit(1); }
    for (int i = 0; i < seg_size; i++) payload[i] = xrand8();

    /* ── send all segments ── */
    double t0    = mono_ms();
    double wall0 = wall_s();
    int failed   = 0;

    for (uint32_t seq = 1; seq <= (uint32_t)NUM_SEGMENTS; seq++) {
        if (send_segment(fd, seq, payload, (uint32_t)seg_size) < 0) {
            fprintf(stderr, "[client] send failed at segment %u\n", seq);
            failed = 1;
            break;
        }
        if (seq % 100 == 0)
            printf("[client] sent %u/%d segments\n", seq, NUM_SEGMENTS);
    }

    double rtt_ms   = 0.0;
    double wall_recv = wall0;
    uint32_t last_seq = 0;

    if (!failed) {
        /* ── await OK response ── */
        if (recv_ok_response(fd, &last_seq) < 0) {
            fprintf(stderr, "[client] failed to receive OK response\n");
            failed = 1;
        } else {
            double t1 = mono_ms();
            wall_recv = wall0 + (t1 - t0) / 1000.0;
            rtt_ms    = t1 - t0;
            printf("[client] received OK — last_seq=%u  RTT=%.2f ms\n", last_seq, rtt_ms);
        }
    }

    close(fd);

    /* ── write CSV ── */
    FILE *fp = fopen(output_path, "w");
    if (!fp) { perror("fopen output"); exit(1); }
    fprintf(fp, "run_id,num_segments,seg_size_bytes,last_seq_acked,"
                "rtt_ms,t_send,t_recv,power_save,ps_set_ok,iface\n");
    fprintf(fp, "%s,%d,%d,%u,%.4f,%.9f,%.9f,%d,%d,%s\n",
        run_id, NUM_SEGMENTS, seg_size, last_seq,
        rtt_ms, wall0, wall_recv,
        power_save, ps_ok, iface);
    fclose(fp);

    printf("[client] results -> %s\n", output_path);
    free(payload);
    return failed ? 1 : 0;
}
