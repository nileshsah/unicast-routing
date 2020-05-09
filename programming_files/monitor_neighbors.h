#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>

#define MAX_NODE 256
#define INF 2147483647
#define BROADCAST_TIMEOUT_IN_SEC 2
#define NUM_CLIENT_THREADS 20

void* announceToNeighbors(void* unusedParam);
void* nodeLivelinessCron(void* unusedParam);

void init();
void listenForNeighbors();

