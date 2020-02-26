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
#include "monitor_neighbors.h"


extern short globalMyID;
//last time you heard from each node. TODO: you will want to monitor this
//in order to realize when a neighbor has gotten cut off from you.
extern struct timeval globalLastHeartbeat[MAX_NODE];

extern char *logFileName;

//our all-purpose UDP socket, to be bound to 10.1.1.globalMyID, port 7777
extern int globalSocketUDP;
//pre-filled for sending to 10.1.1.0 - 255, port 7777
extern struct sockaddr_in globalNodeAddrs[MAX_NODE];

// LSP data
extern long costTable[MAX_NODE][MAX_NODE];


void buildRoutingTable(int);
int isAlive(int);
int buildLinkStatePacket(char *sendBuf, short srcId, short destId, int cost, int clockVal);
void hackyBroadcast(const char* buf, int length, int onlyNeighbors);
void writeToFile(char *buffer);
void *connectionHandler(void *threadId);


int lamportClock;
int lastTableBuildTime;
int clockTable[MAX_NODE][MAX_NODE];
int nextHop[MAX_NODE];
int isNeighbor[MAX_NODE];
int exists[MAX_NODE];

void init() {
	int i, j;
	lamportClock = 1;
    lastTableBuildTime = 0;
	for (i = 0; i < MAX_NODE; i++) {
		for (j = 0; j < MAX_NODE; j++) {
			costTable[i][j] =  -1;
			clockTable[i][j] = 0;
		}
	}
	for (i = 0; i < MAX_NODE; i++) {
		isNeighbor[i] = 0;
		nextHop[i] = -1;
        exists[i] = 0;
	}
	
	FILE *fp = fopen(logFileName, "a");
	fclose(fp);
}

// Logging helper functions

void writeToFile(char *buffer) {
    FILE *fp = fopen(logFileName, "a");
    fwrite(buffer, sizeof(char), strlen(buffer), fp);
    fclose(fp);
}

void logSendRequest(int destId, int nodeId, char *message) {
    char buffer[1000];
    memset(buffer, '\0', sizeof buffer);
    sprintf(buffer, "sending packet dest %d nexthop %d message %s\n", destId, nodeId, message);
    writeToFile(buffer);
}

void logForwardingRequest(int destId, int nodeId, char *message) {
    char buffer[1000];
    memset(buffer, '\0', sizeof buffer);
    sprintf(buffer, "forward packet dest %d nexthop %d message %s\n", destId, nodeId, message); 
    writeToFile(buffer);
}

void logReceiveRequest(char *message) {
	char buffer[1000];
    memset(buffer, '\0', sizeof buffer);
    sprintf(buffer, "receive packet message %s\n", message);
	fprintf(stderr, "%s\n", buffer);

    writeToFile(buffer);
}

void logUnreachableSendRequest(int destId) {
	char buffer[1000];
    memset(buffer, '\0', sizeof buffer);
    sprintf(buffer, "unreachable dest %d\n", destId);
    writeToFile(buffer);
}

// --------------

int updateClock(int clockVal) {
	if (lamportClock < clockVal) {
		lamportClock = clockVal;
	}
    return lamportClock;
}

void updateEdgeCost(int srcId, int destId, int cost, int clockVal) {
	long lastCost;
	lastCost = costTable[srcId][destId];
	
	costTable[srcId][destId] = cost;
	costTable[destId][srcId] = cost;

	clockTable[srcId][destId] = clockVal;
	clockTable[destId][srcId] = clockVal;

    exists[srcId] = exists[destId] = 1;

	if (lastCost != cost) {
        int lamportClockVal = updateClock(lamportClock + 1);
		char buffer[1000]; int length;
		length = buildLinkStatePacket(buffer, srcId, destId, cost, clockVal);
		hackyBroadcast(buffer, length, 1);
		buildRoutingTable(lamportClockVal);
	}
}

int isAlive(int nodeId) {
	struct timeval currentTime;
	gettimeofday(&currentTime, 0);
	long elapsedHeartbeatTime = currentTime.tv_sec - globalLastHeartbeat[nodeId].tv_sec;
	//fprintf(stderr, "%d checked %d = %ld sec\n", globalMyID, nodeId, elapsedHeartbeatTime);
	return (elapsedHeartbeatTime < BROADCAST_TIMEOUT_IN_SEC);
}

void calculateNextHop(int parent[]) {
	int i;
	for (i = 0; i < MAX_NODE; i++) {
		if (i == globalMyID) continue;
		int current = parent[i], last = i;
		while (current != -1) {
			if (current == globalMyID) {
				nextHop[i] = last;
				break;
			}
			last = current;
			current = parent[current];
		}
	}
}

// Single source shortest path using Bellman-Ford algorithm
void buildRoutingTable(int lamportClockVal) {
    if (lastTableBuildTime >= lamportClockVal) {
        return;
    }
    lastTableBuildTime = lamportClockVal;
    long distance[MAX_NODE];
    int i, j, k, n = 0, x, y, parent[MAX_NODE], neighbors[MAX_NODE];
    // Init the distance and parent arrays
    for (i = 0; i < MAX_NODE; i++) {
        distance[i] = INF;
        parent[i] = -1;
        nextHop[i] = -1;
        if (exists[i]) neighbors[n++] = i;
    }
    distance[globalMyID] = 0;

    for (k = 0; k < n; k++) {
        for (x = 0; x < n; x++) for (y = 0; y < n; y++) {
            i = neighbors[x]; j = neighbors[y];
            if (i == j || costTable[i][j] < 0) continue;
            long newCost = distance[i] + costTable[i][j];
            if ((newCost < distance[j]) || (newCost == distance[j] && parent[j] > i)) {
                distance[j] = newCost;
                parent[j] = i;
            }
        }
    }
    calculateNextHop(parent);
}

// lspp<2 bytes srcNode><2 bytes destNode><4 bytes cost><4 bytes nonce>
int buildLinkStatePacket(char *sendBuf, short srcId, short destId, int cost, int clockVal) {
	short noSrcId = htons(srcId), noDestId = htons(destId);
	int noCost = htonl(cost);
	int packetLength = 4 + sizeof(short) + sizeof(short) + sizeof(int) + sizeof(int);

	// Build LSP packet	
	char *writePtr = sendBuf;
	strcpy(writePtr, "lspp");
	
	writePtr += 4;
	memcpy(writePtr, &noSrcId, sizeof(short int));
	
	writePtr += sizeof(short);
	memcpy(writePtr, &noDestId, sizeof(short int));

	writePtr += sizeof(short);
	memcpy(writePtr, &noCost, sizeof(int));


	writePtr += sizeof(int);
	clockVal = (clockVal == -1 ? htonl(lamportClock) : htonl(clockVal));
	memcpy(writePtr, &clockVal, sizeof(int));

	return packetLength;
}

void broadcastLinkStatePacket(short srcId, short destId, int cost) {
	short noSrcId = htons(srcId), noDestId = htons(destId);
	int noCost = htonl(cost), clockVal = htonl(lamportClock);
	int packetLength = 4 + sizeof(short) + sizeof(short) + sizeof(int) + sizeof(int);

	// Build LSP packet
	char sendBuf[packetLength];
	
	char *writePtr = sendBuf;
	strcpy(writePtr, "lspp");
	
	writePtr += 4;
	memcpy(writePtr, &noSrcId, sizeof(short int));
	
	writePtr += sizeof(short);
	memcpy(writePtr, &noDestId, sizeof(short int));

	writePtr += sizeof(short);
	memcpy(writePtr, &noCost, sizeof(int));

	writePtr += sizeof(int);
	memcpy(writePtr, &clockVal, sizeof(int));

	// Broadcast it to all neighbors!
	hackyBroadcast(sendBuf, packetLength, 1);
}

void processLspPacket(short srcId, short destId, int cost, int nonce) {
	int currentClock = clockTable[srcId][destId];
	if (currentClock >= nonce) {
		return;
	}
	updateClock(nonce);
	updateEdgeCost(srcId, destId, cost, nonce);
}


// Yes, this is terrible. It's also terrible that, in Linux, a socket
// can't receive broadcast packets unless it's bound to INADDR_ANY,
// which we can't do in this assignment.
void hackyBroadcast(const char* buf, int length, int onlyNeighbors) {
    int i;
    for (i = 0; i < 256; i++) {
        if (i == globalMyID) continue; // (although with a real broadcast you would also get the packet yourself)
        if (onlyNeighbors && !isNeighbor[i]) continue;
        sendto(globalSocketUDP, buf, length, 0, (struct sockaddr*)&globalNodeAddrs[i], sizeof(globalNodeAddrs[i]));
    }
}

void* announceToNeighbors(void* unusedParam) {
	struct timespec sleepFor;
	sleepFor.tv_sec = 0;
	sleepFor.tv_nsec = 400 * 1000 * 1000; // 500 ms
	while (1) {
		updateClock(lamportClock + 1);
		int clockVal = htonl(lamportClock);
		char beatBuf[4+sizeof(int)];
		strcpy(beatBuf, "beat");
		memcpy(beatBuf+4, &clockVal, sizeof(int));
		hackyBroadcast(beatBuf, 4+sizeof(int), 0);
		nanosleep(&sleepFor, 0);
	}
}

void shareTopologyWithNode(int nodeId) {
    int i, j;
    for (i = 0; i < MAX_NODE; i++) for (j = i + 1; j < MAX_NODE; j++) {
        if (costTable[i][j] < 0) continue;
        char lspBuf[1000]; int length;
        length = buildLinkStatePacket(lspBuf, i, j, costTable[i][j], clockTable[i][j]);
        sendto(globalSocketUDP, lspBuf, length, 0, 
                    (struct sockaddr*)&globalNodeAddrs[nodeId], sizeof(globalNodeAddrs[nodeId]));
    }
}

void* nodeLivelinessCron(void* unusedParam) {
	struct timespec sleepFor;
	sleepFor.tv_sec = 0;
	sleepFor.tv_nsec = 500 * 1000 * 1000; // 500 ms
	
	int i;
	int lastAlive[MAX_NODE];
	memset(lastAlive, 0, sizeof lastAlive);

	while (1) {
		for (i = 0; i < MAX_NODE; i++) {
			int current = isAlive(i);
			if (current != lastAlive[i]) {
				int currentCost = costTable[globalMyID][i];
				updateClock(lamportClock + 1);
				if (current == 0) {
					fprintf(stderr, "%d has found %d dead with cost %d\n", globalMyID, i, currentCost);
					isNeighbor[i] = 0;
					updateEdgeCost(globalMyID, i, -abs(currentCost), lamportClock);
				} else {
					fprintf(stderr, "%d has found %d alive with cost %d\n", globalMyID, i, currentCost);
					isNeighbor[i] = 1;
					updateEdgeCost(globalMyID, i, abs(currentCost), lamportClock);
					shareTopologyWithNode(i);					
				}
			}
			lastAlive[i] = current;
		}
		nanosleep(&sleepFor, 0);
	}
}

// Format: “cost”<4 ASCII bytes>destID<net order 2 bytes>newCost<net order 4 bytes>
void parseManagerCostCommand(char *buffer, short int *nodeId, int *costValue) {
    buffer = buffer + 4;
    memcpy(nodeId, buffer, sizeof(short int));
	*nodeId = ntohs(*nodeId);
	buffer += sizeof(short int);
    memcpy(costValue, buffer, sizeof(int));
	*costValue = ntohl(*costValue);

	fprintf(stderr, "%d: COST nodeId %d %d\n", globalMyID, *nodeId, *costValue);
}

// Format: “send”<4 ASCII bytes>destID<net order 2 bytes>message<some ASCII message (shorter than 100 bytes)>
void parseManagerSendCommand(char *buffer, short *nodeId, char *message, int bufferLen) {
    buffer = buffer + 4;
    memcpy(nodeId, buffer, sizeof(short));
    *nodeId = ntohs(*nodeId);
	buffer += sizeof(short);
	int messageLen = bufferLen - 4 - sizeof(short);
    memcpy(message, buffer, messageLen);

	fprintf(stderr, "%d: SEND nodeId %d %s\n", globalMyID, *nodeId, message);
}

int parseHeartbeatMessage(char *buffer) {
	int clockVal;
	buffer = buffer + 4;
	memcpy(&clockVal, buffer, sizeof(int));
    clockVal = ntohs(clockVal);
	return clockVal;
}

void parseLspPacketMessage(char *buffer, short *srcId, short *destId, int *cost, int *nonce) {
	buffer = buffer + 4;
    memcpy(srcId, buffer, sizeof(short));
    *srcId = ntohs(*srcId);

	buffer += sizeof(short);
	memcpy(destId, buffer, sizeof(short));
    *destId = ntohs(*destId);

	buffer += sizeof(short);
	memcpy(cost, buffer, sizeof(int));
    *cost = ntohl(*cost);

	buffer += sizeof(int);
	memcpy(nonce, buffer, sizeof(int));
    *nonce = ntohl(*nonce);

	fprintf(stderr, "%d: LSPP src:%d dest:%d cost:%d %d\n", globalMyID, *srcId, *destId, *cost, *nonce);
}

void listenForNeighbors() {
    long i, returnVal;
    pthread_t sniffer_thread[NUM_CLIENT_THREADS + 1];
    for (i = 0; i < NUM_CLIENT_THREADS; i++) {
        returnVal = pthread_create(&sniffer_thread[i] , NULL ,  connectionHandler , (void*) i);
        if (returnVal < 0) {
            perror("could not create thread");
            return;
        }
    }
    pthread_exit(NULL);
    return;
}

void *connectionHandler(void *threadId) {
    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen;
    char recvBuf[1005];

    int bytesRecvd;
    while (1) {
        theirAddrLen = sizeof(theirAddr);
        // ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
        bytesRecvd = recvfrom(globalSocketUDP, recvBuf, 1000 , 0, (struct sockaddr*)&theirAddr, &theirAddrLen);
        if (bytesRecvd == -1) {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        recvBuf[bytesRecvd] = '\0';
        //fprintf(stderr, "MyId: %d bytes %d %s\n", globalMyID, bytesRecvd, recvBuf);

        // inet_ntop - convert IPv4 and IPv6 addresses from binary to text form
        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);
        
        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1.")) {
            heardFrom = atoi(strchr(strchr(strchr(fromAddr,'.')+1,'.')+1,'.')+1);
            
            //TODO: this node can consider heardFrom to be directly connected to it; do any such logic now.
            isNeighbor[heardFrom] = 1;

            //record that we heard from heardFrom just now.
            gettimeofday(&globalLastHeartbeat[heardFrom], 0);
        }
        
        //Is it a packet from the manager? (see mp2 specification for more details)
        //send format: 'send'<4 ASCII bytes>, destID<net order 2 byte signed>, <some ASCII message>
        if (!strncmp(recvBuf, "send", 4)) {
            // Sends the requested message to the requested destination node
            //pthread_mutex_lock(&lock);

            short nodeId, nextNodeId;
            char message[100];
            memset(message, '\0', sizeof message);

            parseManagerSendCommand(recvBuf, &nodeId, message, bytesRecvd);

            if (nodeId == globalMyID) {
                logReceiveRequest(message);
            } else {
                nextNodeId = nextHop[nodeId];
                if (nextNodeId== -1) {
                    logUnreachableSendRequest(nodeId);
                } else {
                    sendto(globalSocketUDP, recvBuf, bytesRecvd, 0, 
                        (struct sockaddr*)&globalNodeAddrs[nextNodeId], sizeof(globalNodeAddrs[nextNodeId]));
                    if (heardFrom != -1) {
                        logForwardingRequest(nodeId, nextNodeId, message);			
                    } else {
                        logSendRequest(nodeId, nextNodeId, message);			
                    }
                }
            }
            
            //pthread_mutex_unlock(&lock);
        } else if (!strncmp(recvBuf, "cost", 4)) {
            // 'cost'<4 ASCII bytes>, destID<net order 2 byte signed> newCost<net order 4 byte signed>
            // Records the cost change (remember, the link might currently be down! in that case,
            // this is the new cost you should treat it as having once it comes back up.)
            short int nodeId; int costValue;
            parseManagerCostCommand(recvBuf, &nodeId, &costValue);
            updateClock(lamportClock + 1);
            updateEdgeCost(globalMyID, nodeId, costValue, lamportClock);
        } else if (!strncmp(recvBuf, "beat", 4)) {
            int clockVal = parseHeartbeatMessage(recvBuf);
            updateClock(clockVal + 1);
        } else if (!strncmp(recvBuf, "lspp", 4)) {
            short srcId, destId;
            int cost, nonce;
            parseLspPacketMessage(recvBuf, &srcId, &destId, &cost, &nonce);
            processLspPacket(srcId, destId, cost, nonce);
        }
        
        //TODO now check for the various types of packets you use in your own protocol
        //else if(!strncmp(recvBuf, "your other message types", ))
        // ... 
    }
    //(should never reach here)
    close(globalSocketUDP);
}
