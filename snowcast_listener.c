#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <stdio.h>
#include <inttypes.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma warning(disable:4996)

#else // Linux Unix includes here
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#endif
#define BUFSIZE 1024

// Usage: ./snowcast_listener <udpport>
// create a UDP socket to receive music stream datagram from server
int main(int argc, char* argv[]) {
#if _WIN32
  WORD wVersionRequested = MAKEWORD(2, 2);
  WSADATA wsaData;
  int wsaerr = WSAStartup(wVersionRequested, &wsaData);
#endif

  if (argc != 2) {
    perror("Wrong command! To use, please enter:\n");
    fprintf(stdout, "./snowcast_listener <udpport>\n");
    return -1;
  }

  int sfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); //create a UDP socket to receive music stream datagram
  struct sockaddr_in si_me;
  memset((char *)&si_me, 0, sizeof(si_me));
  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(atoi(argv[1]));
  si_me.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sfd, (struct sockaddr *)&si_me, sizeof(si_me));  //bind socket to a specific port

  char buf[BUFSIZE + 1];
  struct sockaddr_in servinfo;
  int servlen = sizeof(servinfo);
  while (1) {
    memset(buf, '\0', BUFSIZE + 1);
    int received = recvfrom(sfd, buf, BUFSIZE, 0, (struct sockaddr *)&servinfo, &servlen);
    buf[received] = '\0';
    if (received>0)
      write(STDOUT_FILENO, buf, BUFSIZE); //STDOUT_FILENO
  }
  return 0;
}
