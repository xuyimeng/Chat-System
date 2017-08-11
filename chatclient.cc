#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <regex>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

using namespace std;
const regex pattern("(\\d{1,3}.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})");
int portNum;
string IP_Add;

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "*** Author: Yimeng Xu (xuyimeng)\n");
    exit(1);
  }

   // parse input
   string address = argv[1];
   smatch match;
   	bool isMatched = regex_match(address,match,pattern);
   	if(isMatched == false){
   		cerr << "Input argument not match correct pattern. Format <IP address>:<Port>" <<endl;
   		exit(1);
   	}
   	IP_Add = match[1];
   	string port = match[2];
   	portNum = atoi(port.c_str());

   //create a new socket
   	int sock = socket(PF_INET, SOCK_DGRAM, 0);
   	  if (sock < 0) {
   	    fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
   	    exit(1);
   	  }

   //connect to server
	struct sockaddr_in dest;
	bzero(&dest, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(portNum);
	inet_pton(AF_INET, IP_Add.c_str(), &(dest.sin_addr));

	while(true){
		//Initializes the file descriptor set fdset
		fd_set read_fds;
		FD_ZERO(&read_fds);//have zero bits for read file descriptors.
		//prepare read fd sets for select
		FD_SET(sock,&read_fds);
		FD_SET(STDIN_FILENO, &read_fds);

		int ret = select(sock + 1, & read_fds,NULL,NULL,NULL);
		string line;
		// receive command/message from stdin and send it to server
		if(FD_ISSET(STDIN_FILENO, &read_fds)){
			getline(cin,line);
			sendto(sock,line.c_str(),strlen(line.c_str()),0,(struct sockaddr*)&dest,sizeof(dest));
			if(line == "/quit"){
				sleep(1);
				close(sock);
				exit(0);
			}
		}
		//receive message from server
		if(FD_ISSET(sock, &read_fds)){
			char buf[1000];
			struct sockaddr_in src;
			socklen_t srcSize = sizeof(src);
			int rlen = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &srcSize);
			buf[rlen] = 0;
			printf("[%s] (%d bytes) from server\n", buf, rlen);
		}
	}
	return 0;
}  
