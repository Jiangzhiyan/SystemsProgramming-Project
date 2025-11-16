#include <sys/socket.h>	// socket, bind, listen, accept, send
#include <sys/un.h>		// sockaddr_un
#include <sys/types.h>	// pid_t
#include <sys/wait.h>	// waitpid
#include <signal.h>	// sigaction
#include <pthread.h>	// pthread_*
#include <unistd.h>	// close, fork, read, write, unlink
#include <stdlib.h>	// exit, malloc, free
#include <stdio.h>		// printf, perror
#include <string.h>	// memset, strncpy
#include <errno.h>		// errno
#include <stdbool.h>	// bool

#define SOCKET_PATH "/tmp/optimus_prime"
#define CHECKER_THREADS 4

struct prime_request{long int index,number;};
struct prime_reply{long int index,result;};

struct child_state{
	int client_fd;
	int pipe_fd[2];
	pthread_mutex_t prime_mutex;
	pthread_mutex_t print_mutex;
	pthread_mutex_t state_mutex;
	long int prime_counter;
	long int evaluation_counter;
	int fatal_error;
};

struct checker_args{
	struct child_state *state;
	int thread_id;
};

static int listen_fd=-1;
static pid_t server_pid=-1;
static volatile sig_atomic_t sigchld_flag=0;

static void reap_children(void);

static void sigchld_handler(int signo)
	{
	(void)signo;
	sigchld_flag=1;
	}

static void cleanup_socket(void)
	{
	if(getpid()!=server_pid)return;
	if(listen_fd!=-1)
		{
		close(listen_fd);
		listen_fd=-1;
		}
	unlink(SOCKET_PATH);
	}

static int is_prime(long int number)
	{
	if(number<=1)return 0;
	if(number==2)return 1;
	if(number%2==0)return 0;
	for(long int i=3;i<=number/i;i+=2)
		{
		if(number%i==0)return 0;
		}
	return 1;
	}

static int write_full(int fd,const void *buffer,size_t length)
	{
	const char *ptr=(const char*)buffer;
	size_t total=0;
	while(total<length)
		{
		ssize_t written=write(fd,ptr+total,length-total);
		if(written==-1)
			{
			if(errno==EINTR)continue;
			return -1;
			}
		total+=(size_t)written;
		}
	return 0;
	}

static int read_full(int fd,void *buffer,size_t length)
	{
	char *ptr=(char*)buffer;
	size_t total=0;
	while(total<length)
		{
		ssize_t nread=read(fd,ptr+total,length-total);
		if(nread==0)
			{
			return (total==0)?0:-1;
			}
		if(nread==-1)
			{
			if(errno==EINTR)continue;
			return -1;
			}
		total+=(size_t)nread;
		}
	return 1;
	}

static int send_full(int fd,const void *buffer,size_t length)
	{
	const char *ptr=(const char*)buffer;
	size_t total=0;
	while(total<length)
		{
		ssize_t sent=send(fd,ptr+total,length-total,MSG_NOSIGNAL);
		if(sent==-1)
			{
			if(errno==EINTR)continue;
			return -1;
			}
		total+=(size_t)sent;
		}
	return 0;
	}

static void set_fatal_error(struct child_state *state)
	{
	pthread_mutex_lock(&state->state_mutex);
	state->fatal_error=1;
	pthread_mutex_unlock(&state->state_mutex);
	}

static void* checker_thread(void *arg)
	{
	struct checker_args *info=(struct checker_args*)arg;
	struct child_state *state=info->state;
	int thread_id=info->thread_id;
	free(info);

	while(1)
		{
		struct prime_request request;
		ssize_t nread=recv(state->client_fd,&request,sizeof request,MSG_WAITALL);
		if(nread==0)break;
		if(nread==-1)
			{
			if(errno==EINTR)continue;
			perror("recv");
			set_fatal_error(state);
			break;
			}
		if(nread!=sizeof request)
			{
			fprintf(stderr,"[pid %d][thr %d] Incomplete request read (%zd bytes)\n",(int)getpid(),thread_id,nread);
			set_fatal_error(state);
			break;
			}

		pthread_mutex_lock(&state->print_mutex);
		printf("[%d:thread %d] request:[ %ld, %ld]\n",
			(int)getpid(),thread_id,request.index,request.number);
		pthread_mutex_unlock(&state->print_mutex);

		int prime=is_prime(request.number);
		long int primes_so_far;

		pthread_mutex_lock(&state->prime_mutex);
		state->evaluation_counter++;
		if(prime)state->prime_counter++;
		primes_so_far=state->prime_counter;
		pthread_mutex_unlock(&state->prime_mutex);

		struct prime_reply reply;
		reply.index=request.index;
		reply.result=prime?1L:0L;

		if(write_full(state->pipe_fd[1],&reply,sizeof reply)==-1)
			{
			perror("write");
			set_fatal_error(state);
			break;
			}

		pthread_mutex_lock(&state->print_mutex);
		printf("[%d:thread %d] reply:[ %ld, (%s)] total of primes found: %ld\n",
			(int)getpid(),thread_id,request.index,(prime?"Yes":"No "),primes_so_far);
		pthread_mutex_unlock(&state->print_mutex);
		}

	return NULL;
	}

struct pending_reply{
	struct prime_reply data;
	struct pending_reply *next;
};

static void free_pending(struct pending_reply *head)
	{
	while(head)
		{
		struct pending_reply *tmp=head->next;
		free(head);
		head=tmp;
		}
	}

static int insert_pending(struct pending_reply **head,const struct prime_reply *reply)
	{
	struct pending_reply *node=(struct pending_reply*)malloc(sizeof *node);
	if(!node)return -1;
	node->data=*reply;
	node->next=NULL;

	if(!*head || reply->index<(*head)->data.index)
		{
		node->next=*head;
		*head=node;
		return 0;
		}

	struct pending_reply *current=*head;
	while(current->next && current->next->data.index<reply->index)
		current=current->next;

	node->next=current->next;
	current->next=node;
	return 0;
	}

static int pop_pending(struct pending_reply **head,long int index,struct prime_reply *out)
	{
	struct pending_reply *prev=NULL;
	struct pending_reply *current=*head;
	while(current)
		{
		if(current->data.index==index)
			{
			if(prev)prev->next=current->next;
			else *head=current->next;
			*out=current->data;
			free(current);
			return 1;
			}
		prev=current;
		current=current->next;
		}
	return 0;
	}

static int send_reply(struct child_state *state,const struct prime_reply *reply)
	{
	if(send_full(state->client_fd,reply,sizeof *reply)==-1)
		{
		perror("send");
		set_fatal_error(state);
		return -1;
		}
	return 0;
	}

static void* sorting_thread(void *arg)
	{
	struct child_state *state=(struct child_state*)arg;
	struct pending_reply *pending=NULL;
	long int expected_index=0;

	while(1)
		{
		struct prime_reply reply;
		int status=read_full(state->pipe_fd[0],&reply,sizeof reply);
		if(status==0)break;
		if(status==-1)
			{
			perror("read");
			set_fatal_error(state);
			break;
			}

		if(reply.index==expected_index)
			{
			if(send_reply(state,&reply)==-1)break;
			expected_index++;
			while(pop_pending(&pending,expected_index,&reply))
				{
				if(send_reply(state,&reply)==-1)
					{
					free_pending(pending);
					return NULL;
					}
				expected_index++;
				}
			}
		else
			{
			if(insert_pending(&pending,&reply)==-1)
				{
				perror("malloc");
				set_fatal_error(state);
				break;
				}
			}
		}

	/* Flush remaining pending replies, if any */
	while(pending)
		{
		struct prime_reply reply;
		if(!pop_pending(&pending,expected_index,&reply))
			{
			fprintf(stderr,"[pid %d] Missing reply for index %ld during flush\n",(int)getpid(),expected_index);
			break;
			}
		if(send_reply(state,&reply)==-1)break;
		expected_index++;
		}

	free_pending(pending);
	return NULL;
	}

static void initialize_child_state(struct child_state *state,int client_fd)
	{
	memset(state,0,sizeof *state);
	state->client_fd=client_fd;
	state->prime_counter=0L;
	state->evaluation_counter=0L;
	state->fatal_error=0;

	if(pipe(state->pipe_fd)==-1)
		{
		perror("pipe");
		exit(1);
		}

	pthread_mutex_init(&state->prime_mutex,NULL);
	pthread_mutex_init(&state->print_mutex,NULL);
	pthread_mutex_init(&state->state_mutex,NULL);
	}

static void destroy_child_state(struct child_state *state)
	{
	pthread_mutex_destroy(&state->prime_mutex);
	pthread_mutex_destroy(&state->print_mutex);
	pthread_mutex_destroy(&state->state_mutex);
	}

static void run_child(int client_fd)
	{
	struct child_state state;
	initialize_child_state(&state,client_fd);

	pthread_t checker_threads[CHECKER_THREADS];
	pthread_t sort_thread;

	if(pthread_create(&sort_thread,NULL,sorting_thread,&state)!=0)
		{
		perror("pthread_create");
		exit(1);
		}

	for(int i=0;i<CHECKER_THREADS;i++)
		{
		struct checker_args *args=(struct checker_args*)malloc(sizeof *args);
		if(args==NULL)
			{
			perror("malloc");
			exit(1);
			}
		args->state=&state;
		args->thread_id=i;
		if(pthread_create(&checker_threads[i],NULL,checker_thread,args)!=0)
			{
			perror("pthread_create");
			exit(1);
			}
		}

	printf("[%d:child] waiting end of checking threads\n",(int)getpid());
	for(int i=0;i<CHECKER_THREADS;i++)
		{
		pthread_join(checker_threads[i],NULL);
		printf("[%d:child] checking thread %d ended\n",(int)getpid(),i);
		}

	printf("[%d:child] closing pipe (write end)\n",(int)getpid());
	if(close(state.pipe_fd[1])==-1)perror("close");

	printf("[%d:child] waiting end of sorting thread\n",(int)getpid());
	pthread_join(sort_thread,NULL);
	printf("[%d:child] sorting thread ended\n",(int)getpid());

	printf("[%d:child] closing pipe (read end)\n",(int)getpid());
	if(close(state.pipe_fd[0])==-1)perror("close");

	printf("[%d:child] closing socket\n",(int)getpid());
	if(close(state.client_fd)==-1)perror("close");
	printf("[%d:child] connection with %s closed\n",(int)getpid(),SOCKET_PATH);

	printf("[%d:child] checked a total of %ld numbers\n",(int)getpid(),state.evaluation_counter);
	printf("[%d:child] found a total of %ld prime numbers\n",(int)getpid(),state.prime_counter);
	printf("[%d:child] terminating\n",(int)getpid());

	destroy_child_state(&state);
	exit(state.fatal_error?1:0);
	}

static void reap_children(void)
	{
	int saved_errno=errno;
	int status;
	pid_t pid;
	while((pid=waitpid(-1,&status,WNOHANG))>0)
		{
		if(WIFEXITED(status))
			printf("[%d:waiting] Process %d ended with exit(%d)\n",(int)getpid(),pid,WEXITSTATUS(status));
		else if(WIFSIGNALED(status))
			printf("[%d:waiting] Process %d terminated by signal %d\n",(int)getpid(),pid,WTERMSIG(status));
		}
	if(pid==-1 && errno!=ECHILD)perror("waitpid");
	errno=saved_errno;
	}

static void setup_signal_handlers(void)
	{
	struct sigaction sa;
	memset(&sa,0,sizeof sa);
	sa.sa_handler=sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags=0;
	if(sigaction(SIGCHLD,&sa,NULL)==-1)
		{
		perror("sigaction");
		exit(1);
		}
	}

int main(void)
	{
	server_pid=getpid();
	atexit(cleanup_socket);
	setup_signal_handlers();

	listen_fd=socket(AF_UNIX,SOCK_STREAM,0);
	if(listen_fd==-1)
		{
		perror("socket");
		exit(1);
		}

	struct sockaddr_un addr;
	memset(&addr,0,sizeof addr);
	addr.sun_family=AF_UNIX;
	strncpy(addr.sun_path,SOCKET_PATH,sizeof addr.sun_path-1);

	unlink(SOCKET_PATH);
	if(bind(listen_fd,(struct sockaddr*)&addr,sizeof addr)==-1)
		{
		perror("bind");
		exit(1);
		}

	if(listen(listen_fd,16)==-1)
		{
		perror("listen");
		exit(1);
		}

	printf("[%d:main] listening on %s\n",(int)getpid(),SOCKET_PATH);
	printf("[%d:main] waiting for connections...\n",(int)getpid());

	while(1)
		{
		if(sigchld_flag)
			{
			reap_children();
			sigchld_flag=0;
			}

		int client_fd=accept(listen_fd,NULL,NULL);
		if(client_fd==-1)
			{
			if(errno==EINTR)continue;
			perror("accept");
			continue;
			}

		pid_t pid=fork();
		if(pid==-1)
			{
			perror("fork");
			close(client_fd);
			continue;
			}

		if(pid==0)
			{
			close(listen_fd);
			run_child(client_fd);
			}
		else
			{
			printf("[%d:main] new child process created pid: %d\n",(int)getpid(),(int)pid);
			printf("[%d:main] waiting for connections...\n",(int)getpid());
			close(client_fd);
			}
		}

	return 0;
	}
