#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

// shared event data (for simplicity)
struct input_event ev;
pthread_mutex_t ev_mutex = PTHREAD_MUTEX_INITIALIZER;  // mutex to protect shared data
pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;  // condition variable for event processing

// flag to signal program termination
volatile sig_atomic_t terminate = 0;

//signal handler to handle termination (like ctrl+c)
void handle_signal(int sig) {
    terminate = 1;  // set the termination flag to true
}

// thread 1: handle the blocking read (mouse events)
void *read_mouse_events(void *arg) {
    int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);  // open the event device
    if (fd == -1) {
        perror("unable to open event device");  // if opening fails, print error
        return NULL;
    }

    while (!terminate) {
        ssize_t n = read(fd, &ev, sizeof(struct input_event));  // read mouse events

        if (n < sizeof(struct input_event)) {  // if reading is incomplete or failed
            if (n == -1 && errno == EAGAIN) {  // no data available, continue waiting
                usleep(1000);  // sleep for a short time to avoid busy-wait
                continue;
            } else {
                perror("error reading event");  // print error if something goes wrong
                break;
            }
        }

        // lock the mutex before accessing shared event data
        pthread_mutex_lock(&ev_mutex);
        // process the event (e.g., mouse movement or button press/release)
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) {
                printf("mouse moved horizontally: %d\n", ev.value);  // horizontal movement
            } else if (ev.code == REL_Y) {
                printf("mouse moved vertically: %d\n", ev.value);  // vertical movement
            }
        } else if (ev.type == EV_KEY) {
            if (ev.code == BTN_LEFT) {
                if (ev.value == 1) {
                    printf("left button pressed\n");  // left button press
                } else if (ev.value == 0) {
                    printf("left button released\n");  // left button release
                }
            } else if (ev.code == BTN_RIGHT) {
                if (ev.value == 1) {
                    printf("right button pressed\n");  // right button press
                } else if (ev.value == 0) {
                    printf("right button released\n");  // right button release
                }
            } else if (ev.code == BTN_MIDDLE) {
                if (ev.value == 1) {
                    printf("middle button pressed\n");  // middle button press
                } else if (ev.value == 0) {
                    printf("middle button released\n");  // middle button release
                }
            }
        }
        pthread_cond_signal(&ev_cond);  // signal the second thread that a new event is ready
        pthread_mutex_unlock(&ev_mutex);

        usleep(10000);  // sleep to simulate waiting time between events
    }

    close(fd);  // close the file descriptor when done
    return NULL;
}

// thread 2: handle other tasks or process events
void *process_events(void *arg) {
    while (!terminate) {
        // wait until a new event is available from the first thread
        pthread_mutex_lock(&ev_mutex);
        pthread_cond_wait(&ev_cond, &ev_mutex);  // wait for the signal from the first thread

        // process the latest event (this is where you would do more processing)
        if (ev.type != 0) {  // check if there was any event
            printf("processing event type: %d\n", ev.type);  // print event type
        }
        pthread_mutex_unlock(&ev_mutex);

        usleep(20000);  // simulate some delay in processing
    }
    return NULL;
}

int main() {
    // set up signal handler for graceful termination (ctrl+c)
    signal(SIGINT, handle_signal);

    pthread_t thread1, thread2;

    // create thread 1 for reading mouse events
    if (pthread_create(&thread1, NULL, read_mouse_events, NULL) != 0) {
        perror("error creating thread 1");  // if thread creation fails, print error
        return 1;
    }

    // create thread 2 for processing events
    if (pthread_create(&thread2, NULL, process_events, NULL) != 0) {
        perror("error creating thread 2");  // if thread creation fails, print error
        return 1;
    }

    // wait for both threads to finish
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    return 0;
}
