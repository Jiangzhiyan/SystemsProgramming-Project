#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#include "include/protocol.h"

#define BACKLOG 16
#define CHECKING_THREADS 4

struct reply_buffer {
    struct prime_reply *items;
    size_t count;
    size_t capacity;
};

static int client_fd = -1;
static int reply_pipe[2] = {-1, -1};
static pthread_t checking_threads[CHECKING_THREADS];
static pthread_t sorting_thread;
static long thread_ids[CHECKING_THREADS];

static pthread_mutex_t prime_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t eval_mutex = PTHREAD_MUTEX_INITIALIZER;
static long prime_counter = 0;
static long evaluation_counter = 0;

static void reply_buffer_init(struct reply_buffer *buffer);
static void reply_buffer_free(struct reply_buffer *buffer);
static void reply_buffer_add(struct reply_buffer *buffer, const struct prime_reply *reply);
static bool reply_buffer_take(struct reply_buffer *buffer, long expected_index, struct prime_reply *reply);
static void reply_buffer_reserve(struct reply_buffer *buffer, size_t capacity);

static void run_server(void);
static void handle_client(int accepted_fd, const char *peer_path);
static void *checking_thread_main(void *arg);
static void *sorting_thread_main(void *arg);
static int read_request(struct prime_request *request);
static int write_reply(const struct prime_reply *reply);
static int send_reply_to_client(const struct prime_reply *reply);
static int is_prime_number(long int number);
static void set_sigchld_handler(void);
static void sigchld_handler(int signo);
static void install_cleanup_handlers(void);
static void cleanup_socket(void);
static void reset_child_state(int accepted_fd);
static void log_message(const char *component, const char *fmt, ...);

int main(void)
{
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal(SIGPIPE)");
        return EXIT_FAILURE;
    }

    install_cleanup_handlers();
    set_sigchld_handler();
    run_server();
    return EXIT_SUCCESS;
}

static void run_server(void)
{
	int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCK_ADDRESS, sizeof(addr.sun_path) - 1);

	unlink(SOCK_ADDRESS);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind");
		close(listen_fd);
		exit(EXIT_FAILURE);
	}

	if (listen(listen_fd, BACKLOG) == -1) {
		perror("listen");
		close(listen_fd);
		exit(EXIT_FAILURE);
	}

	log_message("main", "server listening on %s", SOCK_ADDRESS);

	for (;;) {
		log_message("main", "waiting for connections...");
		struct sockaddr_un peer_addr;
		socklen_t peer_len = sizeof(peer_addr);
		memset(&peer_addr, 0, sizeof(peer_addr));
		int accepted_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
		if (accepted_fd == -1) {
			if (errno == EINTR) {
				continue;
			}
			perror("accept");
			break;
		}

		const char *peer_name = peer_addr.sun_path[0] != '\0' ? peer_addr.sun_path : "<anonymous>";
		log_message("main", "accepted connection from %s", peer_name);

		pid_t pid = fork();
		if (pid == -1) {
			perror("fork");
			close(accepted_fd);
			continue;
		}

		if (pid == 0) {
			close(listen_fd);
			handle_client(accepted_fd, peer_addr.sun_path);
			_exit(EXIT_SUCCESS);
		}

		close(accepted_fd);
		log_message("main", "new child process created pid: %d", pid);
	}

	close(listen_fd);
}

static void sigchld_handler(int signo)
{
    (void)signo;
    int saved_errno = errno;
    pid_t pid;
    int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (WIFEXITED(status)) {
			log_message("waiting", "Process %d ended with exit(%d)", pid, WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			log_message("waiting", "Process %d terminated by signal %d", pid, WTERMSIG(status));
		}
	}

    errno = saved_errno;
}

static void set_sigchld_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction(SIGCHLD)");
        exit(EXIT_FAILURE);
    }
}

static void install_cleanup_handlers(void)
{
    if (atexit(cleanup_socket) != 0) {
        perror("atexit");
        exit(EXIT_FAILURE);
    }
}

static void cleanup_socket(void)
{
    unlink(SOCK_ADDRESS);
}

static void reset_child_state(int accepted_fd)
{
    client_fd = accepted_fd;
    prime_counter = 0;
    evaluation_counter = 0;
    reply_pipe[0] = -1;
    reply_pipe[1] = -1;
    memset(checking_threads, 0, sizeof(checking_threads));
    memset(thread_ids, 0, sizeof(thread_ids));
}

static void handle_client(int accepted_fd, const char *peer_path)
{
	reset_child_state(accepted_fd);

	char peer_path_copy[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (peer_path && peer_path[0] != '\0') {
		snprintf(peer_path_copy, sizeof(peer_path_copy), "%s", peer_path);
	} else {
		snprintf(peer_path_copy, sizeof(peer_path_copy), "<unbound client>");
	}

	log_message("child", "handling connection from %s", peer_path_copy);

	if (pipe(reply_pipe) == -1) {
		perror("pipe");
		close(client_fd);
		_exit(EXIT_FAILURE);
	}
	log_message("child", "reply pipe created");

	log_message("child", "starting sorting thread");
	int rc = pthread_create(&sorting_thread, NULL, sorting_thread_main, NULL);
	if (rc != 0) {
		errno = rc;
		perror("pthread_create(sorting_thread)");
		close(reply_pipe[0]);
		close(reply_pipe[1]);
		close(client_fd);
		_exit(EXIT_FAILURE);
	}
	log_message("child", "sorting thread running");

	for (long i = 0; i < CHECKING_THREADS; ++i) {
		thread_ids[i] = i;
		log_message("child", "starting checking thread %ld", thread_ids[i]);
		rc = pthread_create(&checking_threads[i], NULL, checking_thread_main, &thread_ids[i]);
		if (rc != 0) {
			errno = rc;
			perror("pthread_create(checking_thread)");
			pthread_cancel(sorting_thread);
			pthread_join(sorting_thread, NULL);
			close(reply_pipe[0]);
			close(reply_pipe[1]);
			close(client_fd);
			_exit(EXIT_FAILURE);
		}
	}

	log_message("child", "waiting end of checking threads");
	for (int i = 0; i < CHECKING_THREADS; ++i) {
		pthread_join(checking_threads[i], NULL);
		log_message("child", "checking thread %d ended", i);
	}

	log_message("child", "closing pipe (write end)");
	close(reply_pipe[1]);
	reply_pipe[1] = -1;

	log_message("child", "waiting end of sorting thread");
	pthread_join(sorting_thread, NULL);
	log_message("child", "sorting thread ended");

	log_message("child", "closing pipe (read end)");
	close(reply_pipe[0]);
	reply_pipe[0] = -1;

	log_message("child", "closing socket");
	close(client_fd);
	client_fd = -1;
	log_message("child", "connection with %s closed", peer_path_copy);

	log_message("child", "checked a total of %ld numbers", evaluation_counter);
	log_message("child", "found a total of %ld prime numbers", prime_counter);
	log_message("child", "terminating");
}

static void *checking_thread_main(void *arg)
{
	long thread_index = *(long *)arg;
	char component[32];
	snprintf(component, sizeof(component), "thread %ld", thread_index);
	log_message(component, "thread started");

	for (;;) {
		struct prime_request request;
		int rc = read_request(&request);
		if (rc == 0) {
			break;
		}
		if (rc < 0) {
			fprintf(stderr, "PID %d thread %ld: read_request failed\n", getpid(), thread_index);
			break;
		}

		log_message(component, "request:[ %ld, %ld]", request.index, request.number);

		int is_prime = is_prime_number(request.number);
		struct prime_reply reply = {
			.index = request.index,
			.result = is_prime ? 1L : 0L
		};

		long total_primes;
		pthread_mutex_lock(&prime_mutex);
		if (is_prime) {
			prime_counter++;
		}
		total_primes = prime_counter;
		pthread_mutex_unlock(&prime_mutex);

		pthread_mutex_lock(&eval_mutex);
		evaluation_counter++;
		pthread_mutex_unlock(&eval_mutex);

		log_message(component,
			   "reply:[ %ld, (%s)] total of primes found: %ld",
			   request.index,
			   is_prime ? "Yes" : "No ",
			   total_primes);

		if (write_reply(&reply) == -1) {
			fprintf(stderr, "PID %d thread %ld: failed to enqueue reply\n", getpid(), thread_index);
			break;
		}
	}

	return NULL;
}

static void *sorting_thread_main(void *arg)
{
	(void)arg;
	struct reply_buffer buffer;
	reply_buffer_init(&buffer);
	log_message("sorting", "thread started");

    long expected_index = 0;
    struct prime_reply reply;

    for (;;) {
        ssize_t result = read(reply_pipe[0], &reply, sizeof(reply));
        if (result == 0) {
            break;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("sorting_thread read");
            break;
        }

        if ((size_t)result != sizeof(reply)) {
            fprintf(stderr, "sorting_thread: partial reply read\n");
            break;
        }

        if (reply.index == expected_index) {
            if (send_reply_to_client(&reply) == -1) {
                fprintf(stderr, "sorting_thread: failed to send reply\n");
                break;
            }
            expected_index++;

            struct prime_reply pending;
            while (reply_buffer_take(&buffer, expected_index, &pending)) {
                if (send_reply_to_client(&pending) == -1) {
                    fprintf(stderr, "sorting_thread: failed to send pending reply\n");
	reply_buffer_free(&buffer);
	log_message("sorting", "thread exiting");
	return NULL;
}
                expected_index++;
            }
        } else if (reply.index > expected_index) {
            reply_buffer_add(&buffer, &reply);
        } else {
            // Duplicate or stale reply, ignore.
        }
    }

    struct prime_reply pending;
    while (reply_buffer_take(&buffer, expected_index, &pending)) {
        if (send_reply_to_client(&pending) == -1) {
            fprintf(stderr, "sorting_thread: failed to flush pending reply\n");
            break;
        }
        expected_index++;
    }

    reply_buffer_free(&buffer);
    return NULL;
}

static int read_request(struct prime_request *request)
{
    size_t to_read = sizeof(*request);
    char *ptr = (char *)request;

    while (to_read > 0) {
        ssize_t n = recv(client_fd, ptr, to_read, MSG_WAITALL);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            return -1;
        }
        to_read -= (size_t)n;
        ptr += n;
    }

    return 1;
}

static int write_reply(const struct prime_reply *reply)
{
    size_t to_write = sizeof(*reply);
    const char *ptr = (const char *)reply;

    while (to_write > 0) {
        ssize_t n = write(reply_pipe[1], ptr, to_write);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write(pipe)");
            return -1;
        }
        to_write -= (size_t)n;
        ptr += n;
    }

    return 0;
}

static int send_reply_to_client(const struct prime_reply *reply)
{
    size_t to_write = sizeof(*reply);
    const char *ptr = (const char *)reply;

    while (to_write > 0) {
        ssize_t n = send(client_fd, ptr, to_write, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "send: unexpected zero byte write\n");
            return -1;
        }
        to_write -= (size_t)n;
        ptr += n;
    }

    return 0;
}

static int is_prime_number(long int number)
{
    if (number <= 1) {
        return 0;
    }
    if (number <= 3) {
        return 1;
    }
    if (number % 2 == 0 || number % 3 == 0) {
        return 0;
    }
    for (long int i = 5; i * i <= number; i += 6) {
        if (number % i == 0 || number % (i + 2) == 0) {
            return 0;
        }
    }
    return 1;
}

static void reply_buffer_init(struct reply_buffer *buffer)
{
    buffer->items = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

static void reply_buffer_free(struct reply_buffer *buffer)
{
    free(buffer->items);
    buffer->items = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

static void reply_buffer_reserve(struct reply_buffer *buffer, size_t capacity)
{
    if (capacity <= buffer->capacity) {
        return;
    }

    size_t new_capacity = buffer->capacity == 0 ? 4 : buffer->capacity;
    while (new_capacity < capacity) {
        new_capacity *= 2;
    }

    struct prime_reply *new_items = realloc(buffer->items, new_capacity * sizeof(*new_items));
    if (!new_items) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }

    buffer->items = new_items;
    buffer->capacity = new_capacity;
}

static void reply_buffer_add(struct reply_buffer *buffer, const struct prime_reply *reply)
{
    reply_buffer_reserve(buffer, buffer->count + 1);

    size_t pos = 0;
    while (pos < buffer->count && buffer->items[pos].index < reply->index) {
        pos++;
    }

    if (pos < buffer->count) {
        memmove(&buffer->items[pos + 1], &buffer->items[pos],
                (buffer->count - pos) * sizeof(buffer->items[0]));
    }

    buffer->items[pos] = *reply;
    buffer->count++;
}

static bool reply_buffer_take(struct reply_buffer *buffer, long expected_index, struct prime_reply *reply)
{
    if (buffer->count == 0) {
        return false;
    }

    if (buffer->items[0].index != expected_index) {
        return false;
    }

    *reply = buffer->items[0];
    if (buffer->count > 1) {
        memmove(&buffer->items[0], &buffer->items[1],
                (buffer->count - 1) * sizeof(buffer->items[0]));
    }
    buffer->count--;
    return true;
}

static void log_message(const char *component, const char *fmt, ...)
{
	const char *label = component ? component : "unknown";
	char stack_buf[256];
	char *heap_buf = NULL;
	const char *body = stack_buf;
	va_list args;
	va_start(args, fmt);
	int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
	va_end(args);
	if (needed < 0) {
		return;
	}
	if ((size_t)needed >= sizeof(stack_buf)) {
		heap_buf = malloc((size_t)needed + 1);
		if (heap_buf) {
			va_start(args, fmt);
			vsnprintf(heap_buf, (size_t)needed + 1, fmt, args);
			va_end(args);
			body = heap_buf;
		}
	}
	printf("[%ld:%s] %s\n", (long)getpid(), label, body);
	fflush(stdout);
	free(heap_buf);
}
