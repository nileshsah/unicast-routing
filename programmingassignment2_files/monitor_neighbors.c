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

//our all-purpose UDP socket, to be bound to 10.1.1.globalMyID, port 7777
extern int globalSocketUDP;
//pre-filled for sending to 10.1.1.0 - 255, port 7777
extern struct sockaddr_in globalNodeAddrs[MAX_NODE];

// LSP data
int lamportClock;
long costTable[MAX_NODE][MAX_NODE];
int clockTable[MAX_NODE][MAX_NODE];
int nextHop[MAX_NODE];

void init() {
	int i, j;
	lamportClock = 1;
	for (i = 0; i < MAX_NODE; i++) {
		for (j = 0; j < MAX_NODE; j++) {
			costTable[i][j] =  -1;
		}
	}
}

void updateClock(int clockVal) {
	if (lamportClock < clockVal) {
		lamportClock = clockVal;
	}
}

void updateEdgeCost(int srcId, int destId, int cost) {
	long lastCost;
	lastCost = costTable[srcId][destId];
	costTable[srcId][destId] = cost;
	clockTable[srcId][destId] = lamportClock;
	if (lastCost != cost && isAlive(srcId) && isAlive(destId)) {
		buildRoutingTable();
	}
}

int isAlive(int nodeId) {
	struct timeval currentTime;
	gettimeofday(&currentTime, 0);
	long elapsedHeartbeatTime = currentTime.tv_sec - globalLastHeartbeat[nodeId].tv_sec;
	return (elapsedHeartbeatTime < BROADCAST_INTERVAL_IN_SEC + 1);
}

void calculateNextHop(int parent[]) {
	int i;
	for (i = 0; i < MAX_NODE; i++) {
		if (i == globalMyID) continue;
		int current = parent[i], last = -1;
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
void buildRoutingTable() {
	long distance[MAX_NODE];
	int i, j, k, parent[MAX_NODE];
	// Init the distance and parent arrays
	for (i = 0; i < MAX_NODE; i++) {
		distance[i] = INF;
		parent[i] = -1;
	}
	distance[globalMyID] = 0;

	for (k = 0; k < MAX_NODE; k++) {
		for (i = 0; i < MAX_NODE; i++) for (j = 0; j < MAX_NODE; j++) {
			if (i == j || !isAlive(i) || !isAlive(j) || costTable[i][j] == -1) continue;
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
void sendLinkStatePacket(short destId, int cost) {
	short noSrcId = htons(globalMyID), noDestId = htons(destId);
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

	hackyBroadcast(sendBuf, packetLength);
}


// Yes, this is terrible. It's also terrible that, in Linux, a socket
// can't receive broadcast packets unless it's bound to INADDR_ANY,
// which we can't do in this assignment.
void hackyBroadcast(const char* buf, int length) {
	int i;
	for (i = 0; i < 256; i++) {
		if (i == globalMyID) continue; // (although with a real broadcast you would also get the packet yourself)
		sendto(globalSocketUDP, buf, length, 0, (struct sockaddr*)&globalNodeAddrs[i], sizeof(globalNodeAddrs[i]));
	}
}

void* announceToNeighbors(void* unusedParam) {
	struct timespec sleepFor;
	sleepFor.tv_sec = 0;
	sleepFor.tv_nsec = 500 * 1000 * 1000; // 300 ms
	while (1) {
		updateClock(lamportClock + 1);
		int clockVal = htonl(lamportClock);
		char beatBuf[4+sizeof(int)];
		strcpy(beatBuf, "beat");
		memcpy(beatBuf+4, &clockVal, sizeof(int));
		hackyBroadcast(beatBuf, 4+sizeof(int));
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
}

// Format: “send”<4 ASCII bytes>destID<net order 2 bytes>message<some ASCII message (shorter than 100 bytes)>
void parseManagerSendCommand(char *buffer, short *nodeId, char *message) {
    buffer = buffer + 4;
    memcpy(nodeId, buffer, sizeof(short));
    *nodeId = ntohs(*nodeId);
	buffer += sizeof(short);
	int messageLen = strlen(buffer) - 4 - sizeof(short);
    memcpy(message, buffer, messageLen);
}

int parseHeartbeatMessage(char *buffer) {
	int clockVal;
	buffer = buffer + 4;
	memcpy(&clockVal, buffer, sizeof(int));
    clockVal = ntohs(clockVal);
	return clockVal;
}

void listenForNeighbors()
{
	char fromAddr[100];
	struct sockaddr_in theirAddr;
	socklen_t theirAddrLen;
	char recvBuf[1000];

	ssize_t bytesRecvd;
	while(1) {
		theirAddrLen = sizeof(theirAddr);
		// ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
		bytesRecvd = recvfrom(globalSocketUDP, recvBuf, 1000 , 0, (struct sockaddr*)&theirAddr, &theirAddrLen);
		if (bytesRecvd == -1) {
			perror("connectivity listener: recvfrom failed");
			exit(1);
		}
		recvBuf[bytesRecvd] = '\0';

		// inet_ntop - convert IPv4 and IPv6 addresses from binary to text form
		inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);
		
		short int heardFrom = -1;
		if (strstr(fromAddr, "10.1.1.")) {
			heardFrom = atoi(strchr(strchr(strchr(fromAddr,'.')+1,'.')+1,'.')+1);
			
			//TODO: this node can consider heardFrom to be directly connected to it; do any such logic now.
	
			//record that we heard from heardFrom just now.
			int lastState = isAlive(heardFrom);
			gettimeofday(&globalLastHeartbeat[heardFrom], 0);
			if (lastState == 0) {
				buildRoutingTable();
			}
		}
		
		//Is it a packet from the manager? (see mp2 specification for more details)
		//send format: 'send'<4 ASCII bytes>, destID<net order 2 byte signed>, <some ASCII message>
		if(!strncmp(recvBuf, "send", 4)) {
			// Sends the requested message to the requested destination node
			int nodeId;
			char message[100];
			parseManagerSendCommand(recvBuf, &nodeId, message);
			if (nodeId == globalMyID) {
				// Log
			} else {
				sendto(globalSocketUDP, recvBuf, bytesRecvd, 0, 
					(struct sockaddr*)&globalNodeAddrs[nodeId], sizeof(globalNodeAddrs[nodeId]));		
			}
		} else if(!strncmp(recvBuf, "cost", 4)) {
			// 'cost'<4 ASCII bytes>, destID<net order 2 byte signed> newCost<net order 4 byte signed>
			// Records the cost change (remember, the link might currently be down! in that case,
			// this is the new cost you should treat it as having once it comes back up.)
			short int nodeId;
			int costValue;
			parseManagerCostCommand(recvBuf, &nodeId, &costValue);
			updateEdgeCost(globalMyID, nodeId, costValue);
		} else if(!strncmp(recvBuf, "beat", 4)) {
			int clockVal = parseHeartbeatMessage(recvBuf);
			updateClock(clockVal + 1);
		}
		
		//TODO now check for the various types of packets you use in your own protocol
		//else if(!strncmp(recvBuf, "your other message types", ))
		// ... 
	}
	//(should never reach here)
	close(globalSocketUDP);
}

