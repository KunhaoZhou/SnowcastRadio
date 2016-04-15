#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <stdio.h>
#include <inttypes.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>
#include <time.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "pthreadVC2.lib")
#pragma warning(disable:4996)

#else // Linux & Unix include here
#include <stdio.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#endif

#define HELLO 0
#define SETSTATION 1
#define WELCOME 0
#define ANNOUNCE 1
#define INVALID 2

int sfd; // a socket connected to server
uint16_t stationNum; // the number of stations server has

struct ClientCommand{
  uint8_t Type;
  uint16_t num;
};

// Receive announce when a new song is playing
int announceHelper(void* arg) {
  uint8_t type = -1;
  while (1) {
    int received = recv(sfd, &type, 1, 0);
    if (type == INVALID) { // InvalidCommand
      uint8_t replyStringSize = 0;
      received = received + recv(sfd, &replyStringSize, 1, 0);
      char* replyString = malloc((replyStringSize + 1)*sizeof(char));
      memset(replyString, '\0', strlen(replyString));
      received = received + recv(sfd, replyString, replyStringSize, 0);
      replyString[replyStringSize] = '\0';
      fprintf(stdout, "Announce: %s\n", replyString);
      free(replyString);
    }
    else if (type == ANNOUNCE) { // New song Announce
      uint8_t songnamesize = 0;
      char* songname = malloc((songnamesize + 1)*sizeof(char));
      received = received + recv(sfd, &songnamesize, 1, 0);
      memset(songname, '\0', strlen(songname));
      received = received + recv(sfd, songname, songnamesize, 0);
      songname[songnamesize] = '\0';
      fprintf(stdout, "Announce: Now playing %s.\n", songname);
      free(songname);
      if (received != 2 + songnamesize) {
        perror("announceHelper: recv error! Please enter 'q' to quit.\n");
        exit(-1);
      }
    }
    else {
      break;
    }
  }
  return 0;
}

// handle client command, 'q': quit. integer: SETSTATION
int inputHelper(void* arg) {
  while (1) { 
    char input[50];
    scanf("%s", input);
    if ((input[0] == 'q') && (input[1] == '\0')) // quit
      break;
    uint16_t sendStationNum = atoi(input);
    if ((sendStationNum > stationNum) || (sendStationNum <= 0)) {
      fprintf(stdout,
        "Invalid station number: please enter station number from 1 to %d\n", stationNum);
      continue;
    }
    struct ClientCommand SetStation = { SETSTATION, htons(sendStationNum - 1) };
    int sent = send(sfd, &SetStation.Type, 1, 0) + send(sfd, &SetStation.num, 2, 0);
    if (sent != 3)
      perror("send Error!\n");
  }
  return 0;
}

// creat a socket and connect it to server
int setSocket(char* name, char* port) {
  struct addrinfo hints, *res, *p;
  memset((void*)&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(name, port, &hints, &res) != 0) {
    perror("getaddrinfo error");
    return -1;
  }

  sfd = -1;
  for (p = res; p != NULL; p = p->ai_next) { // loop through all the results and connect to the first we can
    sfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol); // return a socket file descriptor 
    if (sfd == -1)
      perror("Socket Error\n");  // fail to creat a socket
    if (connect(sfd, res->ai_addr, res->ai_addrlen) != 0)
      perror("Connecting to server failed\n");
  }

  if (sfd > 0)
    fprintf(stdout,"Connected!\n");
  freeaddrinfo(res);
  return sfd;
}

// send a Hello command to server and get the number of stations server has
int sendHello(int sfd, uint16_t udpport) {
  struct ClientCommand Hello = { HELLO, htons(udpport) };
  int sent = send(sfd, &Hello.Type, 1, 0) + send(sfd, &Hello.num, 2, 0);
  if (sent != 3) {
    perror("send Error!\n");
    return -1;
  }
  uint8_t type = -1;
  stationNum = -1;
  int received = recv(sfd, &type, 1, 0) + recv(sfd, &stationNum, 2, 0);
  stationNum = ntohs(stationNum);
  if (received != 3)
    perror("recv Error!\n");
  if (type != WELCOME)
    perror("Unwelcome...Error!\n");
  if ((type != WELCOME) || (received != 3) || (sent != 3))
    return -1;
  return stationNum;
}



// Usage: snowcast_control <servername> <serverport> <udpport>
int main(int argc, char* argv[]) {
#if _WIN32
  WORD wVersionRequested = MAKEWORD(2, 2);
  WSADATA wsaData;
  int wsaerr = WSAStartup(wVersionRequested, &wsaData);
#endif
  if (argc != 4) {
    perror("Wrong command! To use, please enter:\n");
    perror("./snowcast_control <servername> <serverport> <udpport>\n");
    return -1;
  }
  char* servername = argv[1];
  char* serverport = argv[2];
  uint16_t udpport = atoi(argv[3]);

  setSocket(servername, serverport); // creat a socket and connect it to server
  if (sfd < 0) {
    perror("setSocket Error!\n");
    return -1;
  }

  sendHello(sfd, udpport); // send a Hello command to server and get the number of stations server has
  if (stationNum <= 0) {
    perror("sendHello Error!\n");
    return -1;
  }
  
  pthread_t annouceThread;
  pthread_create(&annouceThread, 0, announceHelper, NULL);
  fprintf(stdout,
    "Welcome: We have %d stations. Please enter station number (1 to %d) to SetStation, or 'q' to quit.\n", 
    stationNum, stationNum);

  pthread_t inputThread;
  pthread_create(&inputThread, 0, inputHelper, NULL);
  pthread_join(inputThread, NULL); // don't quit until a quit command is received in inputThread

  return 0;
}
