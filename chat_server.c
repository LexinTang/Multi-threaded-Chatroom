#include "chat.h"
#include "chat_server.h"
#include <string.h>
#include <signal.h>
#include <assert.h> 
#include <pthread.h>

static char banner[] =
"\n\n\
/*****************************************************************/\n\
/*    Client/Server Application - Mutli-thread Chat Server       */\n\
/*                                                               */\n\
/*    USAGE:  ./chat_server    [port]                            */\n\
/*            Press <Ctrl + C> to terminate the server           */\n\
/*****************************************************************/\n\
\n\n";

/* 
 * Debug option 
 * In case you do not need debug information, just comment out it.
 */

#define DEBUG_PRINT(_f, _a...) \
    do { \
        printf("[debug]<%s> " _f "\n", __func__, ## _a); \
    } while (0)
#else
#define DEBUG_PRINT(_f, _a...) do {} while (0)
#endif


void server_init(void);
void server_run(void);
void *broadcast_thread_fn(void *);
void *client_thread_fn(void *);
int send_msg_to_server(int sockfd, char *msg, int command, int privateData);
void shutdown_handler(int);

#define BACKLOG 10
#define MYPORT 50388

struct chat_server  chatserver;
int port = MYPORT;
int sockfd;  // listen on sock_fd
struct sockaddr_in their_addr; // client's address information
socklen_t sin_size;

struct chatmsg_queue *msgQ = &chatserver.room.chatmsgQ;

sem_t *buf_full = &chatserver.room.chatmsgQ.buffer_full;
sem_t *buf_empty = &chatserver.room.chatmsgQ.buffer_empty;
sem_t *mq_lock = &chatserver.room.chatmsgQ.mq_lock;
sem_t *cq_lock = &chatserver.room.clientQ.cq_lock;

/*
 * The main server process
 */
int main(int argc, char **argv)
{
    printf("%s\n", banner);
    
    if (argc > 1) {
        port = atoi(argv[1]);
    } else {
        port = MYPORT;
    }

    printf("Starting chat server ...\n");

    // Register "Control + C" signal handler
    signal(SIGINT, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

	// Initilize the server
    server_init();
    
	// Run the server
    server_run();

    return 0;
}


/*
 * Initilize the chatserver
 */
void server_init(void)
{
    // Initilize all related data structures
    // 1. semaphores, mutex, pointers, etc.
    // 2. create the broadcast_thread

	int i = 0; 
	while (i < MAX_QUEUE_MSG){
		msgQ->slots[i] = (char *)malloc(sizeof(char) * CONTENT_LENGTH);
		//memset(msgQ->slots[i], '\0', CONTENT_LENGTH);//or memset(msgQ->slots[i], 0, sizeof(char) * CONTENT_LENGTH);
		i++;
	}

	memset(&chatserver.room.clientQ, 0, sizeof(struct client_queue));

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		exit(1);
	}

	/* Prepare the socket address structure of the server */    
	chatserver.address.sin_family = AF_INET;         // host byte order
	chatserver.address.sin_port = htons(port);     // short, network byte order
	chatserver.address.sin_addr.s_addr = htonl(INADDR_ANY); // automatically fill with my IP address
	memset(&(chatserver.address.sin_zero), '\0', 8); // zero the rest of the struct
	
	if (bind(sockfd, (struct sockaddr *)&chatserver.address, sizeof(struct sockaddr)) == -1) {
		perror("bind");
		exit(1);
	}

	printf("Chat server is up and listening at port %d\n", port);

	/* initialize all synchronization structures */
	sem_init(buf_full, 0, MAX_QUEUE_MSG);		//initially, has 20 slots in chatmsgQ
	sem_init(buf_empty, 0, 0);					//initially, no items in chatmsgQ
	sem_init(mq_lock, 0, 1);					//work as mutex lock - initially is free
	sem_init(cq_lock, 0, 1);

	/* create broadcast_thread */
	pthread_create(&(chatserver.room.broadcast_thread), NULL, (void *)(*broadcast_thread_fn), (void *)(msgQ));

} 

int send_msg_to_server(int sockfd, char *msg, int command, int privateData)
{
    struct exchg_msg sbuf;
    int msg_len = 0;

	memset(&sbuf, 0, sizeof(struct exchg_msg));
	sbuf.instruction = htonl(command); 
	if (command == CMD_SERVER_BROADCAST) {          
		memcpy(sbuf.content, msg, strlen(msg));
		msg_len = strlen(msg) + 1;
        msg_len = (msg_len < CONTENT_LENGTH) ? msg_len : CONTENT_LENGTH;
        sbuf.content[msg_len-1] = '\0';	
        sbuf.private_data = htonl(msg_len);
    }
	else sbuf.private_data = htonl(privateData);


    if (send(sockfd, &sbuf, sizeof(sbuf), 0) == -1) {
        perror("Server socket sending error");
        return -1;
    }

    return 0;
}
/*
 * Run the chat server 
 */
void server_run(void)
{


    while (1) {
        // Listen for new connections
        // 1. if it is a CMD_CLIENT_JOIN, try to create a new client_thread
        //  1.1) check whether the room is full or not
        //  1.2) check whether the username has been used or not
        // 2. otherwise, return ERR_UNKNOWN_CMD

		int new_fd;	//new connection on new_fd
		struct exchg_msg mbuf;	//mbuf for received msg
		int instruction, msg_len;		
		char clientName [CLIENTNAME_LENGTH];//clientName for every distinguish thread
		
		if (listen(sockfd, BACKLOG) == -1) {
			perror("listen");
			exit(1);
		}

		sin_size = sizeof(struct sockaddr_in);
		if ((new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
			perror("accept");
			exit(1);
		}

		/* communicate with the client using new_fd */

		/* receive msg from client */			
		memset(&mbuf, 0, sizeof(struct exchg_msg));
		if (recv(new_fd, &mbuf, CONTENT_LENGTH-1, 0) == -1) {
		    perror("recv error occurs, exit");
		    exit(1);
		}

		/* handle byte endian */
		instruction = ntohl(mbuf.instruction);
		msg_len = ntohl(mbuf.private_data);

		if (instruction == CMD_CLIENT_JOIN){
			assert(msg_len <= CLIENTNAME_LENGTH);
			strcpy(clientName, mbuf.content);

			/* check room ********************************/
			sem_wait(cq_lock);
			if (chatserver.room.clientQ.count >= MAX_ROOM_CLIENT) {
				sem_post(cq_lock);//release lock
				if (send_msg_to_server(new_fd, NULL, CMD_SERVER_FAIL, ERR_JOIN_ROOM_FULL) != 0) exit(1);
				close(new_fd); 
				continue;	//Join unsuccessfully, so have to listen for another join request
			}
			sem_post(cq_lock);

			/* check usename */
			int checkName = 1;
			sem_wait(cq_lock);
			struct chat_client *p = chatserver.room.clientQ.head;				
			while (p != NULL){
				if (strcmp(p -> client_name , clientName) == 0){checkName = 0; break;}
				p = p -> next;
			}
			sem_post(cq_lock);//release lock
			if (checkName == 0) {
				if (send_msg_to_server(new_fd, NULL, CMD_SERVER_FAIL, ERR_JOIN_DUP_NAME) != 0) exit(1);
				close(new_fd); 
				continue;	//Join unsuccessfully, so have to listen for another join request
			}
		 	/* checking finished***************************/

			/* collect client info */
			struct chat_client *newClient;
			newClient = (struct chat_client *)malloc(sizeof(struct chat_client));
			memset(newClient, 0, sizeof(struct chat_client));
			newClient -> socketfd = new_fd;
			newClient -> address = their_addr;
			strcpy(newClient -> client_name, clientName);				
			pthread_create(&(newClient -> client_thread), NULL, (void *)(*client_thread_fn), (void *)(newClient));

		}
		//otherwise, return ERR_UNKNOWN_CMD
		else {
			if (send_msg_to_server(new_fd, NULL, CMD_SERVER_FAIL, ERR_UNKNOWN_CMD) != 0) exit(1);
			close(new_fd); 
			continue;	//Join unsuccessfully, so have to listen for another join request
		}
	}
}


void *client_thread_fn(void *arg)
{
    // Put one message into the bounded buffer "$client_name$ just joins, welcome!"

    struct chat_client *clientInfo;
	clientInfo = arg;

	/* Insert the client into clientQ */
	sem_wait(cq_lock);
	if (chatserver.room.clientQ.tail != NULL){				
		chatserver.room.clientQ.tail -> next = clientInfo;
		clientInfo -> prev = chatserver.room.clientQ.tail;
		chatserver.room.clientQ.tail = clientInfo;}
	else{
		chatserver.room.clientQ.head = clientInfo;
		chatserver.room.clientQ.tail = clientInfo;
		chatserver.room.clientQ.head -> next = NULL;
		chatserver.room.clientQ.tail -> prev = NULL;
	}
	chatserver.room.clientQ.count ++;
	sem_post(cq_lock);	// release lock

	/* enable cancallation and set the thread cancellation state to asynchronous */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	
	struct exchg_msg mbuf;	//mbuf for received msg
	char content[CONTENT_LENGTH]; //content for storing outgoing msg string
	int instruction, msg_len;		
	int new_fd = clientInfo -> socketfd;	


	/* send CMD_SERVER_JOIN_OK back to client */
	if (send_msg_to_server(new_fd, NULL, CMD_SERVER_JOIN_OK, -1) != 0) exit(1);

	/* send welcome message to client */
	memset(&content, '\0', sizeof(content));
	strcat(content, clientInfo -> client_name);	
	strcat(content, " just joins the chat room, welcome!");
	sem_wait(buf_full);			//wait for space
	sem_wait(mq_lock);			//now has space, wait for lock
	strcpy(msgQ->slots[msgQ->tail], content);	//put item in msgQ.slots
	msgQ->tail = (msgQ->tail + 1) % MAX_QUEUE_MSG;
	sem_post(mq_lock);				//release lock
	sem_post(buf_empty);			//indicate one more item in msgQ

	printf("A new client enters [%s %s:%d]\n",clientInfo -> client_name, inet_ntoa(clientInfo -> address.sin_addr), clientInfo -> address.sin_port);
	//sem_wait(cq_lock);	
	//printf("[debug]Clients in chatroom : %d.\n", chatserver.room.clientQ.count);
	//sem_post(cq_lock);

    while (1) {
        // Wait for incomming messages from this client
        // 1. if it is CMD_CLIENT_SEND, put the message to the bounded buffer
        // 2. if it is CMD_CLIENT_DEPART: 
        //  2.1) send a message "$client_name$ leaves, goodbye!" to all other clients
        //  2.2) free/destroy the resources allocated to this client
        //  2.3) terminate this thread
		
		/* receive msg from client */			
		memset(&mbuf, 0, sizeof(struct exchg_msg));
		if (recv(new_fd, &mbuf, CONTENT_LENGTH-1, 0) == -1) {
		    perror("recv error occurs, exit");
		    exit(1);
		}

		/* handle byte endian */
		instruction = ntohl(mbuf.instruction);
		msg_len = ntohl(mbuf.private_data);
		if (instruction == CMD_CLIENT_SEND) {
			assert(msg_len <= CLIENTNAME_LENGTH);

			memset(&content, '\0', sizeof(content));				
			strcat(content, clientInfo -> client_name);	
			strcat(content, ": ");		
			strcat(content, mbuf.content);
			sem_wait(buf_full);			//wait for space
			sem_wait(mq_lock);			//now has space, wait for lock
			strcpy(msgQ->slots[msgQ->tail], content);	//put item in msgQ.slots
			msgQ->tail = (msgQ->tail + 1) % MAX_QUEUE_MSG;
			sem_post(mq_lock);				//release lock
			sem_post(buf_empty);			//indicate one more item in msgQ

		}
		else if (instruction == CMD_CLIENT_DEPART) 
		{	close(new_fd); 

			/* send "Goodbye" msg to every clients */
			memset(&content, '\0', sizeof(content));				
			strcat(content, clientInfo -> client_name);	
			strcat(content, " just leaves the chat room, goodbye!");
			sem_wait(buf_full);			//wait for space
			sem_wait(mq_lock);			//now has space, wait for lock
			strcpy(msgQ->slots[msgQ->tail], content);	//put item in msgQ.slots
			msgQ->tail = (msgQ->tail + 1) % MAX_QUEUE_MSG;
			sem_post(mq_lock);				//release lock
			sem_post(buf_empty);			//indicate one more item in msgQ

			/* remove the client from clientQ, be sure to delete the correct one! */
			sem_wait(cq_lock);
			chatserver.room.clientQ.count --;
			// pay attention to the special cases,such as deleting the head or tail of the list
			if ((clientInfo -> prev != NULL)&&(clientInfo -> next != NULL)){	
				clientInfo -> next -> prev = clientInfo -> prev;
				clientInfo -> prev -> next = clientInfo -> next;
			}
			else if ((clientInfo -> next == NULL)&&(clientInfo -> prev != NULL))
				{clientInfo -> prev -> next = NULL;	chatserver.room.clientQ.tail = clientInfo -> prev;}
			else if ((clientInfo -> prev == NULL)&&(clientInfo -> next != NULL))
				{clientInfo -> next -> prev = NULL;	chatserver.room.clientQ.head = clientInfo -> next;}
			else {chatserver.room.clientQ.head = NULL;	chatserver.room.clientQ.tail = NULL;}
			sem_post(cq_lock);//release lock

			printf("A client departs [%s %s:%d]\n", clientInfo -> client_name, inet_ntoa(clientInfo-> address.sin_addr), clientInfo-> address.sin_port);
			//sem_wait(cq_lock);
			//printf("[debug]Clients in chatroom : %d.\n", chatserver.room.clientQ.count);
			//sem_post(cq_lock);

			pthread_detach(pthread_self());	/* instruct system to automatically remove my thread resource after termination */
			free(clientInfo);		//'newClient' points to the same area 
			pthread_exit(0); 	

		}
   }
} 


void *broadcast_thread_fn(void *arg)
{
	/* enable cancallation and set the thread cancellation state to asynchronous */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (1) {
        // Broadcast the messages in the bounded buffer to all clients, one by one
		
		char content[CONTENT_LENGTH]; //content to store outgoing msg string
		memset(&content, '\0', sizeof(content));

		sem_wait(buf_empty);
		sem_wait(mq_lock);
		strcpy(content, msgQ->slots[msgQ->head]);

		sem_wait(cq_lock);
		struct chat_client *p = chatserver.room.clientQ.head;
		while (p != NULL){	
			if (send_msg_to_server(p -> socketfd, content, CMD_SERVER_BROADCAST, -1) != 0) exit(1);
			p = p -> next;
		}
		sem_post(cq_lock);

		msgQ->head = (msgQ->head + 1) % MAX_QUEUE_MSG;
		sem_post(mq_lock);		
		sem_post(buf_full);
    }
}


/*
 * Signal handler (when "Ctrl + C" is pressed)
 */
void shutdown_handler(int signum)
{
    // Implement server shutdown here
    // 1. send CMD_SERVER_CLOSE message to all clients
    // 2. terminates all threads: broadcast_thread, client_thread(s)
    // 3. free/destroy all dynamically allocated resources: memory, mutex, semaphore, whatever.
	
	
	printf("Kill by SIGKILL (kill -2)\n");
	printf("Shutdown server .....\n");
	/* terminate broadcast_thread */
	pthread_cancel(chatserver.room.broadcast_thread);
	pthread_join(chatserver.room.broadcast_thread, NULL);

	/* send CMD_SERVER_CLOSE message to all clients, terminate all client_thread & free clientQ */
	sem_wait(cq_lock);
	struct chat_client *p = chatserver.room.clientQ.head;		
	while (p != NULL){	
		if (send_msg_to_server(p -> socketfd, NULL, CMD_SERVER_CLOSE, -1) != 0) exit(1);
		pthread_cancel(p -> client_thread);
		pthread_join(p -> client_thread, NULL);
		close(p -> socketfd);	//close all new_fd
		if (p -> next != NULL){
			p = p -> next;
			free(p -> prev);
		}
		else{free(p); break;}
	}
	sem_post(cq_lock);	//release lock
	
	/* free msgQ */
	int i = 0; 
	while (i < MAX_QUEUE_MSG){
		free(msgQ -> slots[i]);
		i++;
	}

	/* destroy mutex, semaphore */
	sem_destroy(buf_full);	//release semaphore resources
	sem_destroy(buf_empty);
	sem_destroy(mq_lock);
	sem_destroy(cq_lock);
	printf("Done\n");
    exit(0);
}
