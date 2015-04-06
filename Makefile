all: chat_client chat_server

chat_client: chat_client.o
	gcc chat_client.o -o chat_client -pthread -lncurses

chat_client.o: chat_client.c chat.h
	gcc -c -Wall -g chat_client.c

chat_server: chat_server.o
	gcc chat_server.o -o chat_server -pthread

chat_server.o: chat_server.c chat.h chat_server.h
	gcc -c -Wall -g chat_server.c

clean:
	rm -rf *.o
	rm -rf chat_client chat_server
