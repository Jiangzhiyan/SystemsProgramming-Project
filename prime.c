//File: prime.c
//Descritpion:
// This is the provided client for the SHU 2025/2026 Systems Programming
// project assignment. 
// The client creates an UNIX stream oriented socket, binds a local
// address to it and connects to the server. Upon a successful connection
// it starts reading numbers from stdin and, for each number, sends the 
// corresponding prime evaluation request to the server.
// When the corresponding prime evaluation replies are received, they are 
// printed on the screen (an evaluation is made to check is they were 
// received out-of-order).
// When the client detects that the stdin was closed, it waits for prime 
// evaluation replies that may be pending, and then closes the socket and 
// terminates.
//Implementation details:
// The main reads the numbers from stdin and sends the prime evaluation 
// requests to the server. The reading of the prime evaluation replies, 
// from the server, is done by a thread, called reading_thread.
// The reading thread only reads from the socket when instructed by the 
// main thread. This is done through a pipe from the main thread to the 
// reading thread. This way, the reading thread waits for a reading command 
// from the pipe, and only when a reading command if received from the pipe, 
// the reading thread reads a prime evaluation reply from the socket.
//
// The reason for doing it this way deserves some attention.
// The socket used to send the prime evaluation request, to the server, and
// receive the prime evaluation replies, from the server, has one file 
// descriptor, returned by the socket system call, used both for the writing 
// and reading operations. Once closed, can no longer be used neither for 
// writing, nor reading.
// Well, the client signals the server the end of the requests by closing the 
// connected socket. It should do that only after all the replies have been 
// received. 
// On the other end, it is not a good programming practice to close a file 
// descriptor that another thread may be using (in our case the reading 
// thread). 
// It is not a question of style. In some cases, may lead to undesired 
// results. Imagine a thread that is about to read from a device, with a 
// given file descriptor. In the meantime, the OS switches to another thread 
// that closes that file descriptor, and before returning to the original 
// thread another thread gets the same file descriptor reassigned by the OS 
// (reused). When the original thread resumes with the intended read on the 
// file descriptor is going to read from a different device than originally 
// intended. In our case, by having the reading thread waiting commands on 
// the pipe, the main thread, on termination, may close its end of the pipe. 
// As the commands are being read from the pipe (by the reading thread), at a 
// given point (after all replies have been received) the reading thread will 
// get an EOF (end-of-file) condition on the pipe and may close its end and 
// terminate. Now, the main thread, on detecting the end of the reading 
// thread, may close the socket. 
//
//Notes:
// 1. The binding is not a requirement in order to have bidirectional
// communication between the client and the server (using Unix stream 
// sockets). But, binding the client address allows the server to know 
// (information) its client address (identification).
// 3. The send system call is invoked with the MSG_NOSIGNAL flag, otherwise,
// if the connection was lost in the meantime the process would receive a
// SIGPIPE signal that would kill it. This way (with the MSG_NOSIGNAL flag)
// the send would return -1 and the errno external variable would be set to
// EPIPE  (broken pipe).
#include <sys/socket.h>	// socket, AF_UNIX, bind, connect, recv, send
#include <sys/un.h>		// sockaddr_un - man page unix(7)
#include <stdlib.h>		// exit
#include <stdio.h>		// printf, perror, fgets, sprintf, fprintf
#include <unistd.h>		// unlink, close, getpid, pipe
#include <string.h>		// strcpy, strlen, memset
#include <pthread.h>	// pthread_create, pthread_join
#define SOCK_ADDRESS "/tmp/optimus_prime"

//Global variables
int sock_fd;//socket file descriptor
int pipe_fd[2];//read from pipe[0]; write to pipe_fd[1]
long int reply_counter=0L;
long int prime_counter=0L;

struct prime_request{long int index,number;};
struct prime_reply{long int index,result;};

void* reading_thread(void* arg)
	{
	while(1)
		{
		//read command from the pipe
		char c;//there is only one command: R (so, c is not checked)
		int nread=read(pipe_fd[0],&c,sizeof c);
		if(nread==-1){perror("Error: reading pipe");exit(1);}
		else if(nread==0)break;//EOF

		//a read command was received, so read reply from socket
		struct prime_reply reply;
		nread=recv(sock_fd,&reply,sizeof reply,MSG_WAITALL);
		if(nread==-1){perror("Error: reading socket");exit(1);}
		else if(nread==0){perror("Error: unexpected socket EOF");exit(1);}
		else if(nread!=sizeof reply)
			{perror("Error: wrong number of bytes read");exit(1);}

		//print results
		printf("[%3ld,%10s]\t%s\n",reply.index,
			(reply.result==1L)?"(Yes)":"(No )",
			(reply.index==reply_counter)?"":"(out-of-order)");

		//increment counters
		if(reply.result==1L)prime_counter++;
		reply_counter++;
		}
	return NULL;
	}//reading_thread

int main()
	{
	int retcode;

	//create a UNIX stream oriented socket
	sock_fd=socket(AF_UNIX,SOCK_STREAM,0); //domain:AF_UNIX; type:SOCK_STREAM
	if(sock_fd==-1){perror("Error: socket");exit(1);}
	
	//bind a local address to the socket
	struct sockaddr_un local_client_addr;
	memset(&local_client_addr,0,sizeof local_client_addr);
	local_client_addr.sun_family=AF_UNIX;
	sprintf(local_client_addr.sun_path, "%s_%d",SOCK_ADDRESS,getpid());
	printf("The socket local address is %s\n",local_client_addr.sun_path);
	unlink(local_client_addr.sun_path);
	if(bind(sock_fd,(struct sockaddr*)&local_client_addr,sizeof local_client_addr)==-1)
		{perror("Error: bind");exit(1);}

	//connect to the server
	struct sockaddr_un server_addr;
	memset(&server_addr,0,sizeof server_addr);
	server_addr.sun_family=AF_UNIX;
	strcpy(server_addr.sun_path, SOCK_ADDRESS);
	if(connect(sock_fd,(const struct sockaddr*)&server_addr,sizeof server_addr)==-1)
		{perror("Error: connect");exit(1);}
	printf("Connection established with %s\n",server_addr.sun_path);

	//create a pipe to send read commands to the reading thread
	if(pipe(pipe_fd)==-1){perror("Error: pipe");exit(1);}
	
	//create reading thread
	pthread_t thread_id;
	retcode=pthread_create(&thread_id,NULL,reading_thread,NULL);
	if(retcode!=0){fprintf(stderr,"Error(%d): pthread_create",retcode);exit(1);}

	long int index=0L;
	while(1)
		{
		//read stdin
		long int number;
		int retcode=scanf("%ld",&number);
		if(retcode==0)break;//not a number
		else if(retcode==EOF)break;//end of file

		//write prime evaluation requests to socket and increment index
		struct prime_request request;
		request.index=index++;
		request.number=number;
		int nwritten=send(sock_fd,&request,sizeof request,MSG_NOSIGNAL);
		if(nwritten==-1){perror("send");exit(1);}
		else if(nwritten!=sizeof request)
			{perror("Error: wrong number of bytes written");exit(1);}
		printf("[%3ld,%10ld]\n",request.index,request.number);
		
		//instruct reading thread to read the server reply
		if(write(pipe_fd[1],"R",1)==-1)
			{perror("Error: writing pipe");exit(1);}
		}//while(1)
	
	//close pipe write end (on success, reader will see EOF)
	if(close(pipe_fd[1])==-1){perror("Error: close");exit(1);}

	//wait end of thread
	retcode=pthread_join(thread_id,NULL);
	if(retcode!=0){fprintf(stderr,"Error(%d): pthread_join",retcode);exit(1);}

	//close pipe read end (pipe no longer needed)
	if(close(pipe_fd[0])==-1){perror("Error: close");exit(1);}

	//close socket
	if(close(sock_fd)==-1){perror("Error: close");exit(1);}
	printf("Connection with %s closed\n",server_addr.sun_path);

	//print final report
	printf("Checked a total of %ld numbers\n",reply_counter);
	printf("Found a total of %ld prime numbers\n",prime_counter);
	//terminate
	printf("Terminating\n");
	exit(0);
	}//main
	
//End of file: prime.c
