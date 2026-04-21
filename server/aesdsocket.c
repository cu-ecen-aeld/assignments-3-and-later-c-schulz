#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

// pre-defined parameters
static const char* filename = "/var/tmp/aesdsocketdata";
static const char* port = "9000";

// global parameters, required globally for signal handler
int socket_fd = 0;
int stream_fd = 0;
int file_fd   = 0;
struct addrinfo *res = NULL;    // malloced within getaddrinfo

void free_and_reset_addrinfo(void)
{
    if (res)
    {
        freeaddrinfo(res);
        res = NULL;
    }
}

void cleanup_before_exit(void)
{
    // free addrinfo
    free_and_reset_addrinfo();

    // close open sockets
    if (file_fd)    close(file_fd);
    if (stream_fd)  close(stream_fd);
    if (socket_fd)  close(socket_fd);

    // delete file
    remove(filename);

    // close syslog
    closelog();
}

void signal_handler(int signum)
{
    // backup/restore errno (always in signal handlers!)
    int errno_tmp = errno;

    // handle only SIGTERM and SIGINT
    if ((signum == SIGTERM) || (signum == SIGINT))
    {
        // print syslog message
        syslog(LOG_DEBUG, "Caught signal, exiting.");

        // cleanup stuff before exiting
        cleanup_before_exit();

        // exit regularly, this is the intended exit point of the program
        exit(0);
    }

    // no effect if we exited before this
    errno = errno_tmp;
}

int fork_off_daemon(void)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        syslog(LOG_ERR, "Error in fork(): %d", errno);
        return -1;
    }
    else if (pid > 0)
    {
        // this is the parent
        // we want to exit the process here directly, child handles everything
        exit(0);
    }
    else
    {
        // this is the child
        // exit function to continue with all the socket stuff
        return 0;
    }
}

void setup_signal_handler(void)
{
    // setup signal handler
    struct sigaction signal_action;
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = signal_handler;

    // register signals
    if (sigaction(SIGTERM, &signal_action, NULL) != 0) {
        syslog(LOG_ERR, "Error registering SIGTERM: %d", errno);
    }
    if (sigaction(SIGINT, &signal_action, NULL) != 0) {
        syslog(LOG_ERR, "Error registering SIGINT: %d", errno);
    }

    // print success to syslog
    syslog(LOG_INFO, "Successfully registered signal handler.");
}

int handle_socket(bool daemon_mode)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    static const int buf_size = 4096;
    char recv_buf[buf_size];
    char send_buf[buf_size];

    // allocate address info for port 9000
    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0)
    {
        syslog(LOG_ERR, "Error in getaddrinfo(): %d", rc);
        cleanup_before_exit();
        return -1;
    }

    // create and open socket
    socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (socket_fd < 0)
    {
        syslog(LOG_ERR, "Error in socket(): %d", errno);
        cleanup_before_exit();
        return -1;
    }

    // set socket option to reuse address
    int socket_opt = 1;
    rc = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &socket_opt, sizeof(socket_opt));
    if (rc != 0)
    {
        syslog(LOG_ERR, "Error in setsockopt(): %d", rc);
        cleanup_before_exit();
        return -1;
    }

    // bind socket to file descriptor
    rc = bind(socket_fd, res->ai_addr, sizeof(struct sockaddr));
    if (rc != 0)
    {
        syslog(LOG_ERR, "Error in bind(): %d", errno);
        cleanup_before_exit();
        return -1;
    }

    // free addrinfo, was allocated in getaddrinfo
    free_and_reset_addrinfo();

    // after binding port, fork off daemon if desired
    if (daemon_mode)
    {
        rc = fork_off_daemon();
        if (rc != 0)
        {
            syslog(LOG_ERR, "Error starting as daemon: %d", rc);
            cleanup_before_exit();
            return -1;
        }
    }

    // listen for connection
    rc = listen(socket_fd, 5);
    if (rc != 0)
    {
        syslog(LOG_ERR, "Error in listen(): %d", errno);
        cleanup_before_exit();
        return -1;
    }

    // open (and create) file
    file_fd = open(filename, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, S_IRWXU | S_IRWXG | S_IRWXO);
    if (file_fd < 0)
    {
        syslog(LOG_ERR, "Error in open(): %d", file_fd);
        cleanup_before_exit();
        return -1;
    }

    // in a loop, forever restart accepting connections
    while (true)
    {
        // accept new connection
        struct sockaddr addr;
        socklen_t addrlen = sizeof(addr);
        stream_fd = accept(socket_fd, &addr, &addrlen);
        if (stream_fd < 0)
        {
            syslog(LOG_ERR, "Error in accept(): %d", errno);
            cleanup_before_exit();
            return -1;
        }

        // print to syslog
        syslog(LOG_INFO, "Accepted connection from %d.%d.%d.%d.", (int)addr.sa_data[2], (int)addr.sa_data[3], (int)addr.sa_data[4], (int)addr.sa_data[5]);

        // forever read data from socket (terminated by break)
        while (true)
        {
            // receive (read) data
            int recv_size = recv(stream_fd, recv_buf, buf_size, 0);
            if (recv_size == 0)
            {
                // regular end of receiving
                break;
            }
            else if (recv_size == -1)
            {
                syslog(LOG_DEBUG, "Error in recv(): %d", errno);
                break;
            }
            else
            {
                // write received data to file
                rc = write(file_fd, recv_buf, recv_size);
                if (rc < recv_size)
                {
                    syslog(LOG_DEBUG, "Error in write(): %d", errno);
                }

                // if end of package is reached, send file via socket
                if (recv_buf[recv_size-1] == '\n') {

                    // seek back to begin of file before reading
                    rc = lseek(file_fd, 0, SEEK_SET);
                    if (rc != 0)
                    {
                        syslog(LOG_DEBUG, "Error in lseek(): %d", errno);
                    }

                    // send full content of file via socket back to client
                    int sz;
                    while ((sz = read(file_fd, send_buf, buf_size)) > 0)
                    {
                        syslog(LOG_DEBUG, "Read %d bytes", (int)sz);
                        rc = send(stream_fd, send_buf, sz, MSG_DONTWAIT);
                        if (rc == -1)
                        {
                            syslog(LOG_DEBUG, "Error in send(): %d", errno);
                        }
                    }
                }

                // don't break, continue receiving
            }
        }

        // close connection and print to syslog
        close(stream_fd);
        stream_fd = 0;
        syslog(LOG_INFO, "Closed connection from %d.%d.%d.%d.", (int)addr.sa_data[2], (int)addr.sa_data[3], (int)addr.sa_data[4], (int)addr.sa_data[5]);
    }

    cleanup_before_exit();  // closes all fds
    return 0;
}

int main(int argc, char **argv)
{
    // open syslog
    openlog(NULL, 0, LOG_USER);

    // setup signal handler
    setup_signal_handler();

    // parse arguments to detect daemon mode parameter
    bool daemon_mode = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d"))
            daemon_mode = true;
    }

    // execute the socket stuff
    return handle_socket(daemon_mode);
}