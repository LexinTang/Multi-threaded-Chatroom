#ifndef _CHSERVER_H_
#define _CHSERVER_H_

#include <semaphore.h>

/*
 * Chat server variables
 */
#define DEFAULT_LISTEN_PORT 3500    // the default port number of server/client communication
#define BACKLOG 10                  // the queue length of waiting connections

/*
 * Data structure to store client information
 */
struct chat_client {
    struct chat_client *next, *prev;
    int socketfd;                           // the socket to communicate with client
    struct sockaddr_in address;	            // remote client address
    char client_name[CLIENTNAME_LENGTH];    // remote client username
    pthread_t client_thread;                // the client thread to receive messages, and then put them in the bounded buffer
};

/*
 * Use double-linked list to store all clients
 */
struct client_queue {
#define MAX_ROOM_CLIENT	20      // max. # of clients allowed
    volatile int count;
    struct chat_client *head, *tail;
    sem_t cq_lock; // mutex lock for accessing the link list (you can use pthread_mutex if you like)
};


/*
 * Use bounded buffer to store chat messages, implemented as a circular array and manipulate in FIFO fashion
 * Multiple producers: all client_threads put messages to this buffer
 * Single consumer: the broadcast_thread fetches each message and then sends it to all clients
 */
struct chatmsg_queue {
#define MAX_QUEUE_MSG	20              // size of the bounded buffer of the chat room
    char *slots[MAX_QUEUE_MSG];

    volatile int head;  // pointer to the first message - update by the consumer (boradcast thread)
    volatile int tail;  // pointer to the next available message slot - update by the producers (client threads)
    
    sem_t buffer_empty; // indicate whether the buffer is empty: no message, consumer has to wait
    sem_t buffer_full;  // indicate whether the buffer is full: no available slot, producers have to wait
    sem_t mq_lock;      // mutex lock for accessing the bounded buffer (you can use pthread_mutex if you like)
};


/*
 * Data structure to store room information
 */
struct chat_room {
    struct chatmsg_queue chatmsgQ;  // the message buffer to queue up chat message for broadcast
    struct client_queue clientQ;	// the corresponding slave thread for each client
    pthread_t broadcast_thread;     // the broadcast thread for sending out messages to all clients
};


/*
 * Data structure to store chat_server information
 */
struct chat_server {
    struct sockaddr_in address;     // the server's internet address 
    struct chat_room room;
};

#endif
