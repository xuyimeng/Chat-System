#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <regex>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <set>

using namespace std;

bool verbalMode = false;
// order state
const int UNORDER = 0;
const int FIFO = 1;
const int TOTAL = 2;
const int MAX_NUM_ROOM = 10;
const int BUFF_SIZE = 1000;
// initialize order as unordered
int order = 0;
int serverIdx = 0;
// maps to store relationship
unordered_map<int,string> serverForwardIp;//store server idx : forwarding address
unordered_map<int,int> serverForwardPort;//store server idx : forwarding port num
unordered_map<int,set<string> > chatRooms; //room id : set of client ip:port
unordered_map<string,int> clientRoomMap; //client ip:port : room number
unordered_map<string,string> nickname; //client ip:port : nick name

int totalServerNum = 0; //total number of server
int curtServerIdx = 0; // first server index is 1
string curtServerIp = "";
int curtServerPort;
const regex addPattern("(\\d{1,3}.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})");

// function declaration
string getTime();
void sendMsg(int sock, struct sockaddr_in src,string msg);
void deliverMessage(int sock,int roomNum, string message);
void multicastMsg(int sock, string message);

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "*** Author: Yimeng Xu (xuyimeng)\n");
		exit(1);
	}
	// parse optional arguments
	int index, c;
	while((c = getopt(argc,argv,"vo:"))!=-1){
		switch(c){
		case 'v':
			verbalMode = true;
			continue;
		case 'o':
			if(strcmp(optarg,"unordered") == 0){
				order = UNORDER;
			} else if (strcmp(optarg, "fifo") == 0) {
				order = FIFO;
			} else if (strcmp(optarg, "total") == 0) {
				order = TOTAL;
			} else {
				cerr << "-ERR:ordering mode Invalid" << endl;
				exit(1);
			}
			continue;
		default:
			cerr << "-ERR:Unknown option "<<	endl;
			exit(1);
		}
	}

	if(optind != argc - 2){
		cerr <<"-ERR: Need to provide 2 arguments: configuration file address and server index"<<endl;
		exit(1);
	}
	// parse argument configFile and server Index
	index = optind;

	FILE* configFile;
	configFile = fopen(argv[index],"r");
	if(configFile == NULL){
		cout<<"Error could not open file"<<argv[index]<<endl;
		exit(1);
	}
	index++;
	// save server index
	curtServerIdx = atoi(argv[index]);
	// parse configFile and store forward IP of all servers
	char* line = NULL;
	size_t len = 0;
	ssize_t read;
	int lineIdx = 1;

	if(verbalMode){
		cout << getTime() <<"Parse config file..."<<endl;
	}

	while((read = getline(&line, &len, configFile)) != -1){
		string str(line);
		string forwardAdd;
		string bindAdd;
		size_t separator = str.find(',');
		// separate two address of each line
		if(separator == string::npos){
			cout << "Server " << lineIdx <<" has same bind forward address"<<endl;
			forwardAdd = str;
			bindAdd = str;
		}else{
			forwardAdd =  str.substr(0,separator);
		    bindAdd = str.substr(separator+1,str.length());
		}
		// curtServer need to save bind address,other server save forward address
		if(lineIdx == curtServerIdx){
			// match address wit regex and get IP and port number
			if(bindAdd[bindAdd.length() - 1] == '\n'){
				bindAdd = bindAdd.substr(0,bindAdd.length() - 1);
			}
			smatch match;
			bool isMatched = regex_match(bindAdd,match,addPattern);
			if(isMatched == false){
				cerr << "Config file not correct address pattern <IP address>:<Port>" <<endl;
				exit(1);
			}
			curtServerIp = match[1];
			string port = match[2];
			curtServerPort = atoi(port.c_str());
			if(verbalMode){
				cout << getTime() << "Current Server IP: "<< curtServerIp <<" port: "<<curtServerPort<< endl;
			}
		}else{
			smatch match;
			bool isMatched = regex_match(forwardAdd,match,addPattern);
			if(isMatched == false){
				cerr <<"Config file not correct address pattern <IP address>:<Port>" <<endl;
				exit(1);
			}

			serverForwardIp[lineIdx] = match[1];
			string port = match[2];
			serverForwardPort[lineIdx] = atoi(port.c_str());
			if(verbalMode){
				cout <<getTime() << "add server " << lineIdx << " with forward IP: "<< serverForwardIp[lineIdx]
				      << " and port num "<< serverForwardPort[lineIdx]<<endl;
			}
		}
		lineIdx++;
	}

	totalServerNum = lineIdx - 1;
	if(verbalMode){
		cout<<getTime() <<"Total server number: "+to_string(totalServerNum)<<endl;
	}

	if(curtServerIp.length() == 0){
		cerr << "Invalid server index" << endl;
		exit(1);
	}

	// initialize chat rooms
	for(int i=1 ; i <= MAX_NUM_ROOM; i++){
		chatRooms[i] = set<string>();
	}

	// current server create socket and bind it to current
	// server address and port number
	int sock = socket(PF_INET, SOCK_DGRAM, 0);

	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(curtServerIp.c_str());
	servaddr.sin_port = htons(curtServerPort);
	bind(sock, (struct sockaddr*)&servaddr, sizeof(servaddr));

	while (true) {
		// server listen for datagram message send from client or other servers
		struct sockaddr_in src;
		socklen_t srclen = sizeof(src);
		char buf[BUFF_SIZE];
		int rlen = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &srclen);
		buf[rlen] = 0;

		//get messgae
		string data(buf);

		// get sender ip and port
		string senderIp = string(inet_ntoa(src.sin_addr));
		int senderPort = ntohs(src.sin_port);
		string senderKey = senderIp + ":"+to_string(senderPort);

		cout << senderPort <<endl;

		// check if the received message belongs to server
		bool fromServerFlag = false;
		int senderIdx = 0;
		for(int i = 1; i <= serverForwardIp.size();i++){
			if(i != curtServerIdx && senderIp == serverForwardIp[i]
			                      && senderPort == serverForwardPort[i]){
				fromServerFlag = true;
				senderIdx = i;
				break;
			}
		}

		if(fromServerFlag){//message from server
			if(order == UNORDER){// message: room number,message
				cout << "In server************"<<endl;
				int roomNum = atoi(data.substr(0,data.find(',')).c_str());

				if(roomNum <= 0){
					cerr << getTime() << "message format wrong (room number invalid)"<<endl;
					continue;
				}else if(roomNum > MAX_NUM_ROOM){
					cerr << getTime() << "room number invalid"<<endl;
					continue;
				}
				string message = data.substr(data.find(',')+1);
				if(verbalMode){
					cout<<getTime()<< "Receive message" <<data<<" from other server"<<endl;
				}
				deliverMessage(sock,roomNum,message);
			}
		}
		else{
			cout << "In client ************" <<endl;
				if(data.size() == 0) continue;
				//check if client has joined a room

				//append croupNum,<client name> before message
				int roomNum = clientRoomMap[senderKey];
				string response = to_string(roomNum) + ",";
				response += data;
				if(order == UNORDER){
//						sendMsg(sock,src,"<"+clientName + "> "+data);
					deliverMessage(sock,roomNum,data);
					multicastMsg(sock,response);
				}

		}
//
//		printf("Echoing [%s] to %s\n", buf, inet_ntoa(src.sin_addr));
//		sendto(sock, buf, rlen, 0, (struct sockaddr*)&src, sizeof(src));
	}
	return 0;
}  

string getTime() {
	char buffer[40];
	struct timeval time;
	gettimeofday(&time, NULL);
	time_t curtime = time.tv_sec;
	strftime(buffer, 40, "%T.", localtime(&curtime));
	string serverIdx = "";
	if(curtServerIdx < 10){
		serverIdx = "0" + to_string(curtServerIdx);
	}else{
		serverIdx = to_string(curtServerIdx);
	}
    return string(buffer) + to_string(time.tv_usec) + " S" + serverIdx + " ";
}

// send message to src destination
void sendMsg(int sock, struct sockaddr_in src,string msg){
	sendto(sock,msg.c_str(),strlen(msg.c_str()),0,(struct sockaddr*)&src, sizeof(src));
}

//deliver message to all client in room = roomNum that current server knows
void deliverMessage(int sock,int roomNum, string message){
	set<string>::iterator it;
	for(it = chatRooms[roomNum].begin();it != chatRooms[roomNum].end();++it){
		//get ip and port of client in that chatroom
		string str = *it;
		string clientIp = str.substr(0,str.find(':'));
		int clientPort = atoi(str.substr(str.find(':')+1).c_str());

		//prepare socket
		struct sockaddr_in dest;
		bzero(&dest, sizeof(dest));
		dest.sin_family = AF_INET;
		dest.sin_port = htons(clientPort);
		inet_pton(AF_INET, clientIp.c_str(), &(dest.sin_addr));

		if(verbalMode){
		cout<<getTime() << "Deliver " <<message << " to " << clientIp<<":"<<to_string(clientPort)<<endl;
		cout<<string(inet_ntoa(dest.sin_addr)) << to_string(ntohs(dest.sin_port))<<endl;
		}

		//send message to client
		sendto(sock,message.c_str(),strlen(message.c_str()),0,(struct sockaddr*)&dest,sizeof(dest));
	}
}

//multicast message to all servers in the system
void multicastMsg(int sock, string message){
	for(unsigned i = 1; i <= totalServerNum; i++){
		if(i == curtServerIdx){
			continue;
		}
		string serverIp = serverForwardIp[i];
		int serverPort = serverForwardPort[i];

		//prepare socket
		struct sockaddr_in dest;
		bzero(&dest, sizeof(dest));
		dest.sin_family = AF_INET;
		dest.sin_port = htons(serverPort);
		inet_pton(AF_INET, serverIp.c_str(), &(dest.sin_addr));

		//send message to client
		sendto(sock,message.c_str(),strlen(message.c_str()),0,(struct sockaddr*)&dest,sizeof(dest));
		if(verbalMode){
			cout<<getTime() << "Multicast " <<message << " to "<< serverIp<<":"<< to_string(serverPort)<<endl;
			cout<<string(inet_ntoa(dest.sin_addr)) << to_string(ntohs(dest.sin_port)) <<endl;
		}
	}
}


