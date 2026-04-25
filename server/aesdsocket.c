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
#include <pthread.h>

#include "queue.h"  // linked list implementation from freebsd-src

// pre-defined parameters
static const char* filename = "/var/tmp/aesdsocketdata";
static const char* port = "9000";

// global parameters, required globally for signal handler
int socket_fd = 0;
int file_fd   = 0;
struct addrinfo *res = NULL;    // malloced within getaddrinfo

// thread parameters
pthread_mutex_t mutex;

struct thread_data
{
    // thread-specific info
    pthread_t thread;
    pthread_mutex_t* mutex_ptr;
    int stream_fd;
    struct sockaddr conn_addr;

    // linked list pointer
    SLIST_ENTRY(thread_data) next;
};

void cleanup_before_exit(void)
{
    // free addrinfo
    if (res)        freeaddrinfo(res);

    // close open sockets
    if (file_fd)    close(file_fd);
    if (socket_fd)  close(socket_fd);

    // delete file
    remove(filename);

    // close syslog
    closelog();

    // no need to reset fds and pointers because we will exit here
    // but let's do it anyways
    file_fd   = 0;
    socket_fd = 0;
    res       = NULL;
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
    // actually fork off
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
        // start new session
        int rc = setsid();
        if (rc < 0)
        {
            syslog(LOG_ERR, "Error in setsid(): %d", errno);
            return -1;
        }

        // change to root directory
        rc = chdir("/");
        if (rc != 0)
        {
            syslog(LOG_ERR, "Error in chdir(): %d", errno);
            return -1;
        }

        // redirect stdin, stdout and stderr to /dev/null
        int devnull_fd = open("/dev/null", O_RDWR);
        if (devnull_fd < 0)
        {
            syslog(LOG_ERR, "Error opening /dev/null: %d", errno);
            return -1;
        }
        dup2(devnull_fd, STDIN_FILENO);
        dup2(devnull_fd, STDOUT_FILENO);
        dup2(devnull_fd, STDERR_FILENO);
        close(devnull_fd);

        // exit function to continue with all the socket stuff
        return 0;
    }
}

int setup_signal_handler(void)
{
    // setup signal handler
    struct sigaction signal_action;
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = signal_handler;

    // register signals
    if (sigaction(SIGTERM, &signal_action, NULL) != 0) {
        syslog(LOG_ERR, "Error registering SIGTERM: %d", errno);
        return -1;
    }
    if (sigaction(SIGINT, &signal_action, NULL) != 0) {
        syslog(LOG_ERR, "Error registering SIGINT: %d", errno);
        return -1;
    }

    // print success to syslog
    syslog(LOG_INFO, "Successfully registered signal handler.");
    return 0;
}

void* handle_timestamp (void*)
{
    while (true)
    {
        // lock mutex before writing to file
        int rc = pthread_mutex_lock(&mutex);
        if (rc != 0)
        {
            syslog(LOG_ERR, "Error in mutex lock(): %d", rc);
            break;
        }

        // get current time and convert to local time
        time_t raw_time;
        time(&raw_time);
        struct tm *ts_info = localtime(&raw_time);
        if (ts_info == NULL)
        {
            syslog(LOG_ERR, "Error getting localtime: %d", errno);
            pthread_mutex_unlock(&mutex);
            break;
        }

        // convert timestamp info to string
        char ts_buf[128];
        int ts_size = strftime(ts_buf, sizeof(ts_buf), "timestamp:%a, %d %b %Y %T %z\n", ts_info);

        // write timestamp data to file
        rc = write(file_fd, ts_buf, ts_size);
        if (rc < ts_size)
        {
            syslog(LOG_DEBUG, "Error in write(): %d", errno);
        }

        // unlock mutex
        rc = pthread_mutex_unlock(&mutex);
        if (rc != 0)
        {
            syslog(LOG_ERR, "Error in mutex unlock(): %d", rc);
            break;
        }

        // sleep 10sec
        usleep (10000000);
    }

    return NULL;
}

void* handle_connection (void* thread_param)
{
    // obtain thread arguments from your parameter
    struct thread_data* thread_args = (struct thread_data *) thread_param;

    // print to syslog
    syslog(LOG_INFO, "Accepted connection from %d.%d.%d.%d.",
        (int)(thread_args->conn_addr).sa_data[2],
        (int)(thread_args->conn_addr).sa_data[3],
        (int)(thread_args->conn_addr).sa_data[4],
        (int)(thread_args->conn_addr).sa_data[5]);

    // buffer for read/write ops
    static const int buf_size = 4096;
    char recv_buf[buf_size];
    char send_buf[buf_size];

    // forever read data from socket (terminated by break)
    while (true)
    {
        // receive (read) data
        int recv_size = recv(thread_args->stream_fd, recv_buf, buf_size, 0);
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
            // lock mutex
            int rc = pthread_mutex_lock(thread_args->mutex_ptr);
            if (rc != 0)
            {
                syslog(LOG_ERR, "Error in mutex lock(): %d", rc);
                break;
            }

            // write received data to file
            rc = write(file_fd, recv_buf, recv_size);
            if (rc < recv_size)
            {
                syslog(LOG_DEBUG, "Error in write(): %d", errno);
            }

            // unlock mutex --> TODO: move this after send?
            rc = pthread_mutex_unlock(thread_args->mutex_ptr);
            if (rc != 0)
            {
                syslog(LOG_ERR, "Error in mutex unlock(): %d", rc);
                break;
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
                    rc = send(thread_args->stream_fd, send_buf, sz, MSG_DONTWAIT);
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
    close(thread_args->stream_fd);
    thread_args->stream_fd = 0;

    // print to syslog
    syslog(LOG_INFO, "Closed connection from %d.%d.%d.%d.",
        (int)(thread_args->conn_addr).sa_data[2],
        (int)(thread_args->conn_addr).sa_data[3],
        (int)(thread_args->conn_addr).sa_data[4],
        (int)(thread_args->conn_addr).sa_data[5]);

    return thread_param;
}

int handle_socket(bool daemon_mode)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

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
        syslog(LOG_ERR, "Error in setsockopt(): %d", errno);
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
    freeaddrinfo(res);
    res = NULL;

    // after binding port, fork off daemon if desired
    if (daemon_mode)
    {
        rc = fork_off_daemon();
        if (rc != 0)
        {
            syslog(LOG_ERR, "Error starting as daemon.");
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
        syslog(LOG_ERR, "Error in open(): %d", errno);
        cleanup_before_exit();
        return -1;
    }

    // initialize linked list
    SLIST_HEAD(thread_list, thread_data);   // defines a struct
    struct thread_list thread_head;         // instantiates the struct
    SLIST_INIT(&thread_head);               // initializes the struct

    // create thread for timestamp writing
    pthread_t time_thread;
    rc = pthread_create(&time_thread, NULL, handle_timestamp, NULL);
    if (rc != 0)
    {
        syslog(LOG_ERR, "Error in pthread_create(): %d", rc);
        cleanup_before_exit();
        return -1;
    }

    // in a loop, forever restart accepting connections
    while (true)
    {
        // accept new connection
        struct sockaddr addr;
        socklen_t addrlen = sizeof(addr);
        int stream_fd = accept(socket_fd, &addr, &addrlen);
        if (stream_fd < 0)
        {
            syslog(LOG_ERR, "Error in accept(): %d", errno);
            cleanup_before_exit();
            return -1;
        }

        // create new thread
        struct thread_data* thread_args = (struct thread_data *) malloc (sizeof (struct thread_data));
        thread_args->mutex_ptr = &mutex;
        thread_args->stream_fd = stream_fd;
        thread_args->conn_addr = addr;

        rc = pthread_create(&(thread_args->thread), NULL, handle_connection, thread_args);
        if (rc != 0)
        {
            syslog(LOG_ERR, "Error in pthread_create(): %d", rc);
            free(thread_args);
            cleanup_before_exit();
            return -1;
        }

        // add thread to linked list
        SLIST_INSERT_HEAD(/* list */&thread_head, /* element */thread_args, /* member name */next);
    }

    // join all threads from linked list
    struct thread_data *item, *tItem;
    SLIST_FOREACH_SAFE(/* element */item, /* list */&thread_head, /* member name */next, /* ? */tItem)
    {
        // join thread, remove from linked list and free the linked list data structure
        pthread_join(item->thread, NULL);
        SLIST_REMOVE(/* list */&thread_head, /* element */item, /* datatype */thread_data, /* member name */next);
        free(item);
    }

    // join timestamp writing thread
    pthread_join(time_thread, NULL);

    cleanup_before_exit();  // closes all fds
    return 0;
}

int main(int argc, char **argv)
{
    // open syslog
    openlog(NULL, 0, LOG_USER);

    // setup signal handler
    int rc = setup_signal_handler();
    if (rc != 0)
    {
        syslog(LOG_ERR, "Error setting up signal handler.");
        cleanup_before_exit();
        return -1;
    }

    // parse arguments to detect daemon mode parameter
    bool daemon_mode = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d"))
            daemon_mode = true;
    }

    // execute the socket stuff
    return handle_socket(daemon_mode);
}