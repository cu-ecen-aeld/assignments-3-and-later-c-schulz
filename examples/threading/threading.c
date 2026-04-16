#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
// #define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    // obtain thread arguments from your parameter
    struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    if (!thread_func_args) {
        ERROR_LOG ("error obtaining thread arguments");
        return thread_param;
    }

    // sleep <wait_to_obtain_ms> ms
    usleep (thread_func_args->wait_to_obtain_us);

    // obtain mutex
    int rc = pthread_mutex_lock (thread_func_args->mutex_ptr);
    if (rc != 0) {
        ERROR_LOG ("error obtaining lock: %d", rc);
        return thread_param;
    }

    // sleep <wait_to_release_ms> ms
    usleep (thread_func_args->wait_to_release_us);

    // set thread success flag with true, protected by mutex
    thread_func_args->thread_complete_success = true;

    // release mutex
    rc = pthread_mutex_unlock (thread_func_args->mutex_ptr);
    if (rc != 0) {
        ERROR_LOG ("error releasing lock: %d", rc);
        return thread_param;
    }

    // return pointer to thread_data structure
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex, int wait_to_obtain_ms, int wait_to_release_ms)
{
    // allocate dynamic memory for thread data
    struct thread_data* thread_func_args = (struct thread_data *) malloc (sizeof (struct thread_data));
    thread_func_args->wait_to_obtain_us       = wait_to_obtain_ms  * 1000;
    thread_func_args->wait_to_release_us      = wait_to_release_ms * 1000;
    thread_func_args->mutex_ptr               = mutex;
    thread_func_args->thread_complete_success = false;

    // create thread
    int rc  = pthread_create (thread, NULL /* default attributes */, threadfunc, thread_func_args);
    if (rc != 0) {
        ERROR_LOG ("error creating thread: %d", rc);
        free (thread_func_args);
        return false;
    }

    return true;
}

