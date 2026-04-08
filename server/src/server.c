/*
 * server.c — TCP server for WiFi power-save RTT experiment
 *
 * Protocol: every message is a 4-byte big-endian length prefix followed
 * by the payload.  The server waits <processing_delay> ms then sends a
 * fixed 2-byte "OK" response (also length-prefixed).
 * A payload length of 0 signals end-of-session from the client.
 *
 * Build:  gcc -O2 -pthread -o server server.c
 * Run:    ./server --port 5001 --delay 10 --log results/server.csv
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

/* ── constants ───────────────────────────────────────────────────────────── */
#define MAX_PAYLOAD     (1 << 20)   /* 1 MiB max message */
#define RESPONSE_BODY   "OK"
#define RESPONSE_LEN    2

/* ── globals shared across threads ─────────────────────────────────────── */
static FILE            *g_log_fp   = NULL;
static pthread_mutex_t  g_log_mtx  = PTHREAD_MUTEX_INITIALIZER;
static volatile int     g_running  = 1;
static double           g_delay_ms = 10.0;   /* processing delay */

/* ── helpers ─────────────────────────────────────────────────────────────── */
static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* recv exactly n bytes; returns 0 on success, -1 on error/EOF */
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

static void ms_sleep(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

/* ── per-client thread ───────────────────────────────────────────────────── */
typedef struct {
    int    fd;
    char   addr[INET_ADDRSTRLEN];
    int    port;
} client_arg_t;

static void *client_thread(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    char addr[INET_ADDRSTRLEN];
    strcpy(addr, ca->addr);
    free(ca);

    uint8_t *payload_buf = malloc(MAX_PAYLOAD);
    if (!payload_buf) { close(fd); return NULL; }

    int msg_idx = 0;

    for (;;) {
        /* ── read 4-byte length header ── */
        uint32_t net_len;
        if (recv_exact(fd, &net_len, 4) < 0) break;
        uint32_t plen = ntohl(net_len);
        if (plen == 0) break;                  /* client signals done */
        if (plen > MAX_PAYLOAD) { fprintf(stderr, "[server] oversized payload %u\n", plen); break; }

        /* ── read payload ── */
        double t_recv = wall_s();
        if (recv_exact(fd, payload_buf, plen) < 0) break;
        double t_payload_done = wall_s();

        /* ── processing delay ── */
        ms_sleep(g_delay_ms);

        /* ── send response ── */
        double t_resp_start = wall_s();
        uint32_t rnet = htonl(RESPONSE_LEN);
        uint8_t resp[4 + RESPONSE_LEN];
        memcpy(resp, &rnet, 4);
        memcpy(resp + 4, RESPONSE_BODY, RESPONSE_LEN);
        if (send(fd, resp, sizeof(resp), MSG_NOSIGNAL) < 0) break;
        double t_resp_sent = wall_s();

        /* ── log ── */
        pthread_mutex_lock(&g_log_mtx);
        if (g_log_fp) {
            fprintf(g_log_fp,
                "%s,%d,%.9f,%.9f,%.9f,%.9f,%.3f\n",
                addr, plen,
                t_recv, t_payload_done, t_resp_start, t_resp_sent,
                g_delay_ms);
            fflush(g_log_fp);
        }
        pthread_mutex_unlock(&g_log_mtx);

        msg_idx++;
    }

    printf("[server] %s disconnected after %d messages\n", addr, msg_idx);
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
        "  -p, --port    <port>   listen port          (default: 5001)\n"
        "  -d, --delay   <ms>     processing delay ms  (default: 10)\n"
        "  -l, --log     <file>   CSV log file         (default: results/server.csv)\n"
        "  -h, --help\n", prog);
    exit(1);
}

int main(int argc, char **argv)
{
    int port = 5001;
    const char *log_path = "results/server.csv";

    static struct option long_opts[] = {
        {"port",  required_argument, 0, 'p'},
        {"delay", required_argument, 0, 'd'},
        {"log",   required_argument, 0, 'l'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:d:l:h", long_opts, &idx)) != -1) {
        switch (c) {
        case 'p': port       = atoi(optarg); break;
        case 'd': g_delay_ms = atof(optarg); break;
        case 'l': log_path   = optarg;       break;
        default:  usage(argv[0]);
        }
    }

    /* create results dir if needed */
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", log_path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    }

    g_log_fp = fopen(log_path, "w");
    if (!g_log_fp) { perror("fopen log"); exit(1); }
    fprintf(g_log_fp,
        "client_addr,payload_bytes,t_recv,t_payload_done,"
        "t_resp_start,t_resp_sent,processing_delay_ms\n");
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

    printf("[server] listening on :%d  delay=%.1f ms  log=%s\n",
           port, g_delay_ms, log_path);

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
