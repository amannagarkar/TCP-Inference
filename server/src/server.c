/*
 * server.c — TCP server for WiFi power-save RTT experiment
 *
 * Protocol: client sends NUM_SEGMENTS segments, each as:
 *   [seq_num (4B BE)] [payload_size (4B BE)] [payload]
 * Server receives all segments in order, then replies:
 *   [4B len prefix]["OK"][4B last_seq BE]
 *
 * Build:  gcc -O2 -pthread -o server server.c
 * Run:    ./server --port 5001 --log results/server.csv
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/stat.h>

/* ── segment protocol ────────────────────────────────────────────────────── */
#define NUM_SEGMENTS    1000        /* expected number of segments per transfer */
#define MAX_SEG_SIZE    (1 << 16)  /* max payload per segment (64 KiB) */

/* ── globals shared across threads ─────────────────────────────────────── */
static FILE            *g_log_fp  = NULL;
static pthread_mutex_t  g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int     g_running = 1;

/* ── helpers ─────────────────────────────────────────────────────────────── */
static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

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

/* send OK response: [4B len prefix]["OK"][4B last_seq BE] (10 bytes total) */
static int send_ok(int fd, uint32_t last_seq)
{
    uint8_t resp[10];
    uint32_t payload_len = htonl(6);   /* "OK" (2) + seq (4) */
    uint32_t net_seq     = htonl(last_seq);
    memcpy(resp,     &payload_len, 4);
    resp[4] = 'O';
    resp[5] = 'K';
    memcpy(resp + 6, &net_seq, 4);
    ssize_t sent = send(fd, resp, sizeof(resp), MSG_NOSIGNAL);
    return (sent == (ssize_t)sizeof(resp)) ? 0 : -1;
}

/* ── per-client thread ───────────────────────────────────────────────────── */
typedef struct {
    int  fd;
    char addr[INET_ADDRSTRLEN];
    int  port;
} client_arg_t;

static void *client_thread(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int  fd = ca->fd;
    char addr[INET_ADDRSTRLEN];
    strcpy(addr, ca->addr);
    free(ca);

    uint8_t *payload_buf = malloc(MAX_SEG_SIZE);
    if (!payload_buf) { close(fd); return NULL; }

    double   t_start   = wall_s();
    uint32_t last_seq  = 0;
    int      seg_count = 0;
    int      error     = 0;

    printf("[server] %s — expecting %d segments\n", addr, NUM_SEGMENTS);

    /* ── receive all segments ── */
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        uint32_t net_seq, net_size;

        if (recv_exact(fd, &net_seq,  4) < 0 ||
            recv_exact(fd, &net_size, 4) < 0) {
            fprintf(stderr, "[server] header recv failed at segment %d\n", i + 1);
            error = 1;
            break;
        }

        uint32_t seq  = ntohl(net_seq);
        uint32_t size = ntohl(net_size);

        if (size > MAX_SEG_SIZE) {
            fprintf(stderr, "[server] oversized segment %u: %u bytes\n", seq, size);
            error = 1;
            break;
        }

        if (recv_exact(fd, payload_buf, size) < 0) {
            fprintf(stderr, "[server] payload recv failed at segment %u\n", seq);
            error = 1;
            break;
        }

        last_seq = seq;
        seg_count++;

        if (seq % 100 == 0)
            printf("[server] received segment %u/%d (%u bytes)\n", seq, NUM_SEGMENTS, size);
    }

    double t_end = wall_s();

    if (!error) {
        /* ── send OK + last_seq ── */
        if (send_ok(fd, last_seq) < 0)
            fprintf(stderr, "[server] failed to send OK to %s\n", addr);
        else
            printf("[server] sent OK last_seq=%u to %s  (%.3f ms)\n",
                   last_seq, addr, (t_end - t_start) * 1e3);
    }

    /* ── log ── */
    pthread_mutex_lock(&g_log_mtx);
    if (g_log_fp) {
        fprintf(g_log_fp, "%s,%d,%u,%.9f,%.9f,%d\n",
                addr, seg_count, last_seq, t_start, t_end, error);
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&g_log_mtx);

    printf("[server] %s disconnected — %d/%d segments received\n",
           addr, seg_count, NUM_SEGMENTS);
    free(payload_buf);
    close(fd);
    return NULL;
}

/* ── signal handler ─────────────────────────────────────────────────────── */
static void sig_handler(int s) { (void)s; g_running = 0; }

/* ── main ───────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -p, --port  <port>  listen port  (default: 5001)\n"
        "  -l, --log   <file>  CSV log file (default: results/server.csv)\n"
        "  -h, --help\n"
        "\nExpects %d segments of [seq, size, payload] per client connection.\n",
        prog, NUM_SEGMENTS);
    exit(1);
}

int main(int argc, char **argv)
{
    int         port     = 5001;
    const char *log_path = "results/server.csv";

    static struct option long_opts[] = {
        {"port", required_argument, 0, 'p'},
        {"log",  required_argument, 0, 'l'},
        {"help", no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:l:h", long_opts, &idx)) != -1) {
        switch (c) {
        case 'p': port     = atoi(optarg); break;
        case 'l': log_path = optarg;       break;
        default:  usage(argv[0]);
        }
    }

    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", log_path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    }

    g_log_fp = fopen(log_path, "w");
    if (!g_log_fp) { perror("fopen log"); exit(1); }
    fprintf(g_log_fp, "client_addr,segments_received,last_seq,t_start,t_end,error\n");
    fflush(g_log_fp);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in saddr = {0};
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) { perror("bind"); exit(1); }
    if (listen(srv, 16) < 0) { perror("listen"); exit(1); }

    printf("[server] listening on :%d  expecting %d segments/transfer  log=%s\n",
           port, NUM_SEGMENTS, log_path);

    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(srv, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int flag = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        client_arg_t *ca = malloc(sizeof(*ca));
        ca->fd   = cfd;
        ca->port = ntohs(caddr.sin_port);
        inet_ntop(AF_INET, &caddr.sin_addr, ca->addr, sizeof(ca->addr));
        printf("[server] connection from %s:%d\n", ca->addr, ca->port);

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, client_thread, ca);
        pthread_attr_destroy(&attr);
    }

    printf("[server] shutting down\n");
    close(srv);
    if (g_log_fp) fclose(g_log_fp);
    return 0;
}
