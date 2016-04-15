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
#include <sys/time.h>
#include <sys/socket.h>
#include <pthread.h>
#include <arpa/inet.h>
#endif
#define MAXCLIENT 100 // Maximum client number this server could support
#define BUFSIZE 1024
#define CYCLE 1.0 / 128 * 1000 // Assume 128kbps mp3
#define HELLO 0
#define SETSTATION 1
#define WELCOME 0
#define ANNOUNCE 1
#define INVALID 2

struct station {
  char* filename;
  char* songname;
  char* buf;
  int written;
  pthread_rwlock_t lock;
  int status;
};

struct client {
  int id;
  char addrs[INET6_ADDRSTRLEN];
  int sfd;
  int udpsfd;
  uint16_t udpport;
  uint16_t stationN;
};

struct station* stations;
uint16_t nstation = 0;
pthread_t* commandThreads;
pthread_t* stationThreads;

// Get ip address
void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET)
    return &(((struct sockaddr_in*)sa)->sin_addr);
  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// Release the lock when a thread is cancelled
int cleanup_routine(pthread_rwlock_t* lock) {
  return pthread_rwlock_unlock(lock);
}

// Send buffer data to udp client
void* streaming(void* client) {
  struct client CL = *(struct client*) client;
  struct sockaddr_in si_client; // set up udp connection
  int slen = sizeof(si_client);
  memset((char *)&si_client, 0, slen);
  si_client.sin_family = AF_INET;
  si_client.sin_port = htons(CL.udpport); //htons
  inet_pton(AF_INET, CL.addrs, &si_client.sin_addr);

  pthread_setcanceltype(PTHREAD_CANCEL_ENABLE, NULL);
  pthread_cleanup_push(pthread_rwlock_unlock, &stations[CL.stationN].lock);

  struct timeval timeout = { 10000, 0 }; // Disconnect UDP if client doesn't receive data over 10s
  setsockopt(CL.udpsfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
  int status = 1;

  while (1) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (pthread_rwlock_rdlock(&stations[CL.stationN].lock) != 0)
      perror("Streaming: rdlock error");
    int sent = sendto(CL.udpsfd, stations[CL.stationN].buf, stations[CL.stationN].written, 0, (struct sockaddr*) &si_client, slen);
    if (sent < stations[CL.stationN].written) {
      pthread_rwlock_unlock(&stations[CL.stationN].lock);
      perror("Streaming: sendto error");
      break;
    }
    if (status != stations[CL.stationN].status) { // it's a new song!
      status = stations[CL.stationN].status;
      uint8_t type = ANNOUNCE;
      uint8_t songnamesize = strlen(stations[CL.stationN].songname);
      sent = send(CL.sfd, &type, 1, 0)
        + send(CL.sfd, &songnamesize, 1, 0)
        + send(CL.sfd, stations[CL.stationN].songname, songnamesize, 0);
      if (sent != 2 + songnamesize) {
        pthread_rwlock_unlock(&stations[CL.stationN].lock);
        perror("Streaming: sending new song announce error");
        break;
      }
    }
    pthread_rwlock_unlock(&stations[CL.stationN].lock);
    do {  // wait till next cycle
      clock_gettime(CLOCK_MONOTONIC, &end);
    } while (((end.tv_sec - start.tv_sec)*1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0) < CYCLE);
  }
  pthread_cleanup_pop(0);
}

// Handle client command. Always expect SetStation command
void* clientCommandHandler(void* client) {
  struct client* CL = (struct client*) client;
  struct client* CLcopy = malloc(sizeof(struct client));
  int stationN, sfd = CL->sfd;
  pthread_t streamThread;
  uint8_t type = -1;
  char* replyString = "client disconnected";
  while (1) {
    int received = recv(sfd, &type, 1, 0) + recv(sfd, &stationN, 2, 0);
    stationN = ntohs(stationN);

    if ((received != 3) || (type != 1)) {
      replyString = "Wrong command received";
      break;
    }
    if (stationN >= nstation) {
      replyString = "Station number error";
      break;
    }
    //Reset timeout
    struct timeval timeout = { 10000000, 0 };
    if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) != 0)
      perror("ClientCommandHandler: setsockopt error");
    if (pthread_rwlock_rdlock(&stations[stationN].lock) != 0)
      perror("ClientCommandHandler: rdlock error");

    uint8_t songnamesize = strlen(stations[stationN].songname);
    type = ANNOUNCE;
    int sent = send(sfd, &type, 1, 0)
      + send(sfd, &songnamesize, 1, 0)
      + send(sfd, stations[stationN].songname, songnamesize, 0);
    if (sent != 2 + songnamesize) {
      replyString = "Send station info error";
      break;
    }
    pthread_rwlock_unlock(&stations[stationN].lock);
    fprintf(stdout, "Station %d is tuned for client %d.\n", stationN, CL->id);

    pthread_cancel(streamThread);
    CL->stationN = stationN;
    *CLcopy = *CL;
    pthread_create(&streamThread, 0, streaming, (void*)CLcopy);
  }
  fprintf(stdout, "Invalid command or client disconnected. client id: %d\n", CL->id);
  type = INVALID;
  uint8_t replyStringSize = strlen(replyString);
  int sent = send(sfd, &type, 1, 0)
    + send(sfd, &(replyStringSize), 1, 0)
    + send(sfd, replyString, replyStringSize, 0);

  // close sockets, udp stream thread, CL
  pthread_cancel(streamThread);
  close(CL->udpsfd);
  close(CL->sfd);
  free(CL);
  free(CLcopy);
}

// Station writes song stream to its buffer
void *playSong(void* s) {
  struct station* station = (struct station*) (s);
  pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
  station->lock = rwlock;
  pthread_rwlockattr_t attr;
  pthread_rwlockattr_init(&attr);
  pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP); // Prefer writer, to avoid writer starvation
  if (pthread_rwlock_init(&station->lock, &attr) != 0) {
    perror("rwlock init error");
    return -1;
  }
  station->status = 0;
  station->buf = malloc((BUFSIZE + 1)*sizeof(char));
  char filename[50];
  strcpy(filename, station->filename);
  int written;
  while (1) {
    FILE* pFile = fopen(filename, "rb");
    if (!pFile) {
      perror("fopen error");
      break;
    }
    do {
      struct timespec start, end;
      clock_gettime(CLOCK_MONOTONIC, &start);
      float loopTime = (float)clock() / CLOCKS_PER_SEC + CYCLE;
      if (pthread_rwlock_wrlock(&station->lock) != 0)
        perror("Playsong: station wrlock error");
      memset(station->buf, '\0', BUFSIZE + 1);
      station->written = fread(station->buf, 1, BUFSIZE, pFile);
      written = station->written;
      station->buf[station->written] = '\0';
      pthread_rwlock_unlock(&station->lock);
      do {  // wait till next cycle
        clock_gettime(CLOCK_MONOTONIC, &end);
      } while (((end.tv_sec - start.tv_sec)*1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0) < CYCLE);
    } while (written == BUFSIZE);
    fclose(pFile);
    pthread_rwlock_wrlock(&station->lock);
    station->status = !station->status;
    pthread_rwlock_unlock(&station->lock);
  }
  free(station->filename);
  free(station->songname);
  free(station->buf);
}

// Accept connection request and expect Hello
void *reception(void* sockfd) {
  int clientid = 0;
  int sfd = *(int*)sockfd;
  fprintf(stdout, "%s%d\n", "server is listening on socket : ", sfd);

  while (1) {
    fprintf(stdout, "server: Ready for new connections...\n");
    struct sockaddr_storage client_addr;
    socklen_t addr_size = sizeof(client_addr);
    int client_sfd = accept(sfd, (struct sockaddr *)&client_addr, &addr_size);
    if (client_sfd == -1) {
      perror("reception: Connection not accepted");
      continue;
    }
    struct timeval timeout = { 10000, 0 };
    if (setsockopt(client_sfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) != 0) {
      perror("reception: setsockopt error");
      continue;
    }

    char client_addrs[INET6_ADDRSTRLEN];
    inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), client_addrs, sizeof client_addrs);
    fprintf(stdout, "server: got connection from %s\n", client_addrs);

    uint8_t type = -1;
    uint16_t udpport = 0;
    int received = recv(client_sfd, &type, 1, 0) + recv(client_sfd, &udpport, 2, 0);
    udpport = ntohs(udpport);
    if ((type != 0) || (received != 3)) {
      perror("reception: Invalid command. Disconnected.");
      type = INVALID;
      char* replyString = "Hello not received.";
      uint8_t replyStringSize = sizeof(replyString);
      int sent = send(client_sfd, &type, 1, 0)
        + send(client_sfd, &(replyStringSize), 1, 0)
        + send(client_sfd, replyString, replyStringSize, 0);
      close(client_sfd);
      continue;
    }

    fprintf(stdout, "Hello from %s is received. Socket id = %i\n", client_addrs, client_sfd);
    type = WELCOME;
    uint16_t nstationx = htons(nstation);
    int sent = send(client_sfd, &type, 1, 0) + send(client_sfd, &nstationx, 2, 0);
    if (sent != 3) {
      fprintf(stdout, "Reply to Client %s failed.", client_addrs);
      close(client_sfd);
      continue;
    }

    fprintf(stdout, "Welcome! Client %i (socket id = %i ) may SetStation\n", clientid, client_sfd);
    int client_udpsfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct client* CL = malloc(sizeof(struct client));
    strncpy(CL->addrs, client_addrs, INET6_ADDRSTRLEN);
    CL->id = clientid;
    CL->sfd = client_sfd;
    CL->udpsfd = client_udpsfd;
    CL->udpport = udpport;
    pthread_create(&commandThreads[clientid], 0, clientCommandHandler, (void*)CL);
    clientid++;
  }
}

// Handle input from server administrator, 'p': playlist, 'q': quit
void *inputHandler() {
  char input[5];
  while (1) { 
    scanf("%s", input);
    if ((input[0] == 'q') && (input[1] == '\0'))
      return;
    if ((input[0] == 'p') && (input[1] == '\0')) {
      fprintf(stdout, "Playlist\n");
      int i = 0;
      for (i = 0; i < nstation; i++) { // loop through to print currently playing songs
        if (pthread_rwlock_rdlock(&stations[i].lock) != 0)
          perror("inputHandler: rdlock error");
        fprintf(stdout, "station%i: %s\n", i, stations[i].songname);
        pthread_rwlock_unlock(&stations[i].lock);
      }
    }
    else
      fprintf(stdout, "Invalid input. Please enter 'q' and 'p' only.\n");
  }
}

// Set up socket to accept clients
int setSocket(char* port) {
  struct addrinfo hints, *res, *p;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(NULL, port, &hints, &res) != 0) {
    perror("getaddrinfo error");
    return -1;
  }

  // make a socket, bind it, and listen on it:  
  int sfd = -1, flag = 1;
  for (p = res; p != NULL; p = p->ai_next) {
    sfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sfd == -1) {
      perror("socket error");
      continue;
    }
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(int)) != 0) {
      perror("setsockopt error");
      continue;
    }
    if (bind(sfd, p->ai_addr, p->ai_addrlen) != 0) {
      perror("bind error");
      close(sfd);
      continue;
    }
    break;
  }
  freeaddrinfo(res);
  return sfd;
}


int main(int argc, char* argv[]) {
  if (argc < 3) {
    perror("Wrong command! To use, please enter:\n");
    perror("./snowcast_server <port> <file1> <file2> ...\n");
    return -1;
  }

  int sfd = setSocket(argv[1]);
  if (listen(sfd, MAXCLIENT) != 0) {
    perror("Failed to listen on a socket.");
    close(sfd);
    return -1;
  }

  // Set up music stations and thread pointers
  nstation = argc - 2;
  stations = malloc(sizeof(struct station)*nstation);
  stationThreads = malloc(sizeof(pthread_t)*nstation);
  commandThreads = malloc(sizeof(pthread_t)*MAXCLIENT);
  int i = 0;
  for (i = 0; i < nstation; i++) {
    stations[i].filename = malloc(strlen(argv[2 + i])*sizeof(char));
    stations[i].filename = argv[2 + i];
    stations[i].songname = malloc((strlen(argv[2 + i]) - 4)*sizeof(char));
    strncpy(stations[i].songname, argv[2 + i], strlen(argv[2 + i]) - 4);
    stations[i].songname[strlen(argv[2 + i]) - 4] = '\0';
    stations[i].written = 0;
    pthread_create(&stationThreads[i], 0, playSong, (void*)&stations[i]);
  }

  // now accept incoming connection
  pthread_t receptionThread, inputThread;
  pthread_create(&receptionThread, 0, reception, &sfd);
  pthread_create(&inputThread, 0, inputHandler, NULL);
  pthread_join(inputThread, NULL); // don't stop until a quit command is received in inputThread

  free(stations);
  free(commandThreads);
  free(stationThreads);
  return 0;
}
