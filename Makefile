CC = gcc
CFLAGS = -Wall -g
LIBS = -lpthread

CLIENT = da
SERVER = daemon

all: $(CLIENT) $(SERVER)

$(CLIENT): analyzer.c structura.h
	$(CC) $(CFLAGS) analyzer.c -o $(CLIENT)

$(SERVER): daemon.c structura.h
	$(CC) $(CFLAGS) daemon.c -o $(SERVER) $(LIBS)

clean:
	rm -f $(CLIENT) $(SERVER)
