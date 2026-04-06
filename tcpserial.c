/*
 * tcpserial.c — TCP-to-serial bridge with userspace XON/XOFF
 *
 * Listens on a TCP port and bridges data to/from a serial port.
 * Handles XON/XOFF flow control in userspace: when XOFF (0x13) is
 * received from the serial device, writing pauses until XON (0x11).
 *
 * Usage: tcpserial /dev/cu.usbserial-110 [port] [baud]
 *   port defaults to 9100, baud defaults to 9600
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define BUF_SIZE   4096
#define XOFF       0x13
#define XON        0x11

/* Outbound queue: TCP data waiting to be written to serial */
#define QUEUE_SIZE (256 * 1024)

static volatile sig_atomic_t g_quit = 0;
static void sig_handler(int sig) { (void)sig; g_quit = 1; }

static int open_serial(const char *path, speed_t baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror(path); return -1; }

    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        perror("tcgetattr"); close(fd); return -1;
    }

    cfmakeraw(&t);
    cfsetispeed(&t, baud);
    cfsetospeed(&t, baud);

    t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;

    /* No kernel flow control — we handle XON/XOFF in userspace */
    t.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK |
                    ISTRIP | INLCR | IGNCR | ICRNL);

    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        perror("tcsetattr"); close(fd); return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int listen_tcp(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 1) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:
        fprintf(stderr, "Unsupported baud rate %d, using 9600\n", baud);
        return B9600;
    }
}

static void bridge(int tcp_fd, int ser_fd)
{
    unsigned char rxbuf[BUF_SIZE];
    unsigned char *queue = malloc(QUEUE_SIZE);
    if (!queue) { perror("malloc"); return; }

    size_t q_head = 0, q_tail = 0;  /* circular queue */
    int paused = 0;                   /* 1 = XOFF received */
    size_t total_tx = 0, total_rx = 0;
    int xoff_count = 0;

    fprintf(stderr, "[tcpserial] Client connected\n");

    /* Send ^ee error query on connect to check printer status */
    {
        const char *ee_cmd = "^ee\r\n";
        write(ser_fd, ee_cmd, strlen(ee_cmd));
        fprintf(stderr, "[tcpserial] Sent ^ee status query\n");
        /* Give printer time to respond before we start sending data */
        usleep(200000);
        ssize_t n = read(ser_fd, rxbuf, sizeof(rxbuf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (rxbuf[i] == XOFF) { paused = 1; xoff_count++; }
                else if (rxbuf[i] == XON) { paused = 0; }
            }
            /* Find the status line (digits, commas, terminated by CR/LF) */
            char status[128] = {0};
            size_t si = 0;
            for (ssize_t i = 0; i < n && si < sizeof(status) - 1; i++) {
                if (rxbuf[i] >= '0' && rxbuf[i] <= '9')
                    status[si++] = rxbuf[i];
                else if (rxbuf[i] == ',' && si > 0)
                    status[si++] = rxbuf[i];
            }
            if (si > 0)
                fprintf(stderr, "[tcpserial] Printer status: %s%s\n",
                        status, strcmp(status, "00") == 0 ? " (OK)" : " (ERROR)");
        }
    }

    while (!g_quit) {
        struct pollfd pfd[2];
        pfd[0].fd = tcp_fd;
        pfd[0].events = (q_head != q_tail || q_head == 0) ? 0 : POLLIN;
        /* Only read more from TCP if queue has space */
        size_t q_used = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail + q_head);
        if (q_used < QUEUE_SIZE - BUF_SIZE)
            pfd[0].events = POLLIN;

        pfd[1].fd = ser_fd;
        pfd[1].events = POLLIN;
        /* If we have data and not paused, also poll for write-ready */
        if (q_head != q_tail && !paused)
            pfd[1].events |= POLLOUT;

        int ret = poll(pfd, 2, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Read from TCP → enqueue */
        if (pfd[0].revents & POLLIN) {
            ssize_t n = read(tcp_fd, rxbuf, sizeof(rxbuf));
            if (n <= 0) {
                /* Client done — drain remaining queue */
                fprintf(stderr, "[tcpserial] Client EOF, draining %zu bytes\n",
                        (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail + q_head));
                while (q_head != q_tail && !g_quit) {
                    /* Check for XON/XOFF while draining */
                    struct pollfd sp = { .fd = ser_fd, .events = POLLIN | POLLOUT };
                    poll(&sp, 1, 100);

                    if (sp.revents & POLLIN) {
                        ssize_t r = read(ser_fd, rxbuf, sizeof(rxbuf));
                        for (ssize_t i = 0; i < r; i++) {
                            if (rxbuf[i] == XOFF) { paused = 1; xoff_count++; }
                            else if (rxbuf[i] == XON) paused = 0;
                            else {
                                total_rx++;
                                fprintf(stderr, "[tcpserial] RX: %02x '%c'\n",
                                        rxbuf[i], (rxbuf[i] >= 0x20 && rxbuf[i] < 0x7f) ? rxbuf[i] : '.');
                            }
                        }
                    }

                    if (paused) { usleep(10000); continue; }

                    if (sp.revents & POLLOUT) {
                        size_t avail = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail);
                        if (avail > 64) avail = 64;  /* small writes for responsiveness */
                        ssize_t w = write(ser_fd, queue + q_tail, avail);
                        if (w > 0) {
                            q_tail = (q_tail + w) % QUEUE_SIZE;
                            total_tx += w;
                        }
                    }
                }
                break;
            }

            /* Enqueue data */
            for (ssize_t i = 0; i < n; i++) {
                queue[q_head] = rxbuf[i];
                q_head = (q_head + 1) % QUEUE_SIZE;
            }
        }

        /* Read from serial — check for XON/XOFF, forward rest to TCP */
        if (pfd[1].revents & POLLIN) {
            ssize_t n = read(ser_fd, rxbuf, sizeof(rxbuf));
            for (ssize_t i = 0; i < n; i++) {
                if (rxbuf[i] == XOFF) {
                    paused = 1;
                    xoff_count++;
                    size_t queued = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail + q_head);
                    fprintf(stderr, "[tcpserial] XOFF #%d (pausing TX, %zu queued)\n",
                            xoff_count, queued);
                    /* Query error status — XOFF can indicate an error */
                    const char *ee_cmd = "^ee\r\n";
                    write(ser_fd, ee_cmd, strlen(ee_cmd));
                } else if (rxbuf[i] == XON) {
                    paused = 0;
                    fprintf(stderr, "[tcpserial] XON (resuming TX)\n");
                } else {
                    /* Real data from printer — forward to TCP */
                    write(tcp_fd, &rxbuf[i], 1);
                    total_rx++;
                    fprintf(stderr, "[tcpserial] RX: %02x '%c'\n",
                            rxbuf[i], (rxbuf[i] >= 0x20 && rxbuf[i] < 0x7f) ? rxbuf[i] : '.');
                }
            }
        }

        /* Write queued data to serial (if not paused) */
        if ((pfd[1].revents & POLLOUT) && !paused && q_head != q_tail) {
            size_t avail = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail);
            if (avail > 64) avail = 64;  /* small chunks so we can react to XOFF quickly */
            ssize_t w = write(ser_fd, queue + q_tail, avail);
            if (w > 0) {
                q_tail = (q_tail + w) % QUEUE_SIZE;
                total_tx += w;
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                perror("write serial");
                break;
            }
        }

        if (pfd[0].revents & (POLLERR | POLLHUP) && q_head == q_tail) break;
        if (pfd[1].revents & POLLERR) break;
    }

    fprintf(stderr, "[tcpserial] Done. TX %zu bytes, RX %zu bytes, %d XOFFs\n",
            total_tx, total_rx, xoff_count);
    free(queue);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: tcpserial <serial-device> [port] [baud]\n");
        return 1;
    }

    const char *serial_path = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : 9100;
    int baud = (argc >= 4) ? atoi(argv[3]) : 9600;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int ser_fd = open_serial(serial_path, baud_const(baud));
    if (ser_fd < 0) return 1;

    int listen_fd = listen_tcp(port);
    if (listen_fd < 0) { close(ser_fd); return 1; }

    fprintf(stderr, "[tcpserial] Listening on port %d → %s @ %d baud (userspace XON/XOFF)\n",
            port, serial_path, baud);

    while (!g_quit) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;

        int tcp_fd = accept(listen_fd, NULL, NULL);
        if (tcp_fd < 0) continue;

        bridge(tcp_fd, ser_fd);
        close(tcp_fd);
        tcflush(ser_fd, TCIOFLUSH);
    }

    close(listen_fd);
    close(ser_fd);
    fprintf(stderr, "[tcpserial] Stopped\n");
    return 0;
}
