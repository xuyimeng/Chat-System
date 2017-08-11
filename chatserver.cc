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
#include <algorithm>
#include <queue>

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

int totalServerNum; //total number of server assigned later
int curtServerIdx = 0; // first server index is 1
string curtServerIp = "";
int curtServerPort;
const regex addPattern("(\\d{1,3}.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})");

// function declaration
string getTime();
void sendMsg(int sock, struct sockaddr_in src,string msg);
void deliverMessage(int sock,int roomNum, string message);
void multicastMsg(int sock, string message);

struct MsgInfo{
	string message;
	int serverID;
	int seqNum;
	string time;
	int pCount; // num of proposal received
	int pMax; // max # of proposaled sequence num
//	int roomNum;
	bool isDeliverable;
};


struct Comp{
    bool operator()(const MsgInfo& m1,const MsgInfo& m2){
    	if(m1.seqNum == m2.seqNum){
    		if(m1.serverID != m2.serverID){
    			return m1.serverID < m2.serverID;
    			//tie breaker,small server Index have higher priority
    		}else{
    			return m1.time < m2.time;
    		}
    	}
    	// sort the holdback queue by seqNum (low to high)
        return m1.seqNum < m2.seqNum ;
    }
};

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

	const int SERVER_NUM = totalServerNum;

	//for FIFO
	int S[MAX_NUM_ROOM + 1] = {}; // S[room id] = counter for message in room I sent by current server
	int R[totalServerNum+1][MAX_NUM_ROOM+1]; //R[N][g] = counter for most recent message from N for room g
	unordered_map<int,unordered_map<int,string> > holdback[MAX_NUM_ROOM+1];
	//holdback[N][g] = a map of msgIdx : message

	for(int m = 0; m <= totalServerNum; m++){
		for(int n = 0; n <= MAX_NUM_ROOM; n++){
			R[m][n] = 0;//R[0]~ never used
		}
	}
	for(int k = 1; k <= MAX_NUM_ROOM; k++){
		for(int l = 1; l <= totalServerNum; l++){
			holdback[k][l] = unordered_map<int,string>();
		}
	}

	//for TOTAL
	// each server maintains two variable
	int P[MAX_NUM_ROOM + 1] = {};//P[g]=Highest sequence number curt server has proposed to group g so far
	int A[MAX_NUM_ROOM + 1] = {};//A[g]=Highest 'agreed' sequence number it has seen for group g
	vector<MsgInfo> holdback_total[MAX_NUM_ROOM+1];//hold back queue for each room contains all message info
//	unordered_map<string,int> pCount[MAX_NUM_ROOM+1]; //map for MsgInfo : current received # of proposal
//	unordered_map<string,int> pMax[MAX_NUM_ROOM+1]; //map for MsgInfo : max proposed sequence number

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

		// check if the received message belongs to server
		bool fromServerFlag = false;
		int senderIdx = 0;
		cout << getTime()<<"senderIp:"<<senderIp <<" senderPort:"<<senderPort <<endl;
		for(int i = 1; i <= totalServerNum;i++){
//			cout <<getTime()<< " i="<<to_string(i)<<" serverIp:port = " << serverForwardIp[i]
//			          <<":"<<serverForwardPort[i];
			if(i != curtServerIdx && senderIp == serverForwardIp[i]
			                      && senderPort == serverForwardPort[i]){
				fromServerFlag = true;
				senderIdx = i;
				break;
			}
		}

		if(fromServerFlag){//message from server
			cout <<"***********In server**************"<<endl;
			if(verbalMode){
				cout <<getTime()<<"Receive message ** "<<data<<" from server #"<<senderIdx<<endl;
			}
			if(order == UNORDER){// message: [room num],[message]
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
			}else if(order == FIFO){//message format:[room num],[message Idx],[message]
				// get room number
				int roomNum = atoi(data.substr(0,data.find(',')).c_str());
				if(roomNum <= 0 || roomNum > MAX_NUM_ROOM ){
					cerr << getTime() << "room number invalid"<<endl;
					continue;
				}
				// get message index
				string msgInfo = data.substr(data.find(',')+1);
				int msgIdx = atoi(msgInfo.substr(0,msgInfo.find(',')).c_str());
				if(msgIdx <= 0){
					cerr << getTime() << "message index invalid"<<endl;
					continue;
				}
				//get message
				string message = msgInfo.substr(msgInfo.find(',')+1);
				if(verbalMode){
					cout <<getTime()<<"FIFO receive server msg: roomNum="
							<<to_string(roomNum)<<" msgIdx:"<<to_string(msgIdx)
							<<" message: "<<message <<endl;
				}
				//put msgIdx:message to holdback queue
				holdback[roomNum][senderIdx][msgIdx] = message;

				//check if msgIdx = R[N][g] + 1
				int nextID = R[senderIdx][roomNum] + 1;

				//check if the desired index is in the holdback queue
				while(holdback[roomNum][senderIdx].find(nextID)
						!= holdback[roomNum][senderIdx].end()){
					string curtMsg = holdback[roomNum][senderIdx][nextID];
					//deliver message to client in the room
					deliverMessage(sock,roomNum,curtMsg);
					holdback[roomNum][senderIdx].erase(nextID);
					nextID = (++R[senderIdx][roomNum]) + 1;
				}
			}else{//total ordering 3 types of data could be received from other server
				// message data: [room number],[message],[time]
				// proposal data: ?[room number],[proposal],[message],[time]
				// decision data: ![room number],[decision],[message],[time]
				if(data[0] == '?'){//receive proposal: ?[room number],[proposal],[message],[time]
					int roomNum = atoi(data.substr(1,data.find(',')).c_str());
					if(roomNum <= 0 || roomNum > MAX_NUM_ROOM ){
						cerr << getTime() << "room number invalid"<<endl;
						continue;
					}
					string restStr = data.substr(data.find(',')+1);
					int proposeNum = atoi(restStr.substr(0,restStr.find(',')).c_str());
					if(proposeNum <= 0){
						cerr << getTime() << "-ERR wrong proposed number"<<endl;
					}
					// server receives other server's proposal
					if(verbalMode){
						cout << getTime()<< "Receive server proposal message: "<<data<<endl;
					}
					string rest2 = restStr.substr(restStr.find(',')+1);
					string propMsg = rest2.substr(0,rest2.find(','));
					string propMsgTime = rest2.substr(rest2.find(',')+1);
					// find the message from holdback queue
					int curtIdx = -1;
					for(int i = 0; i < holdback_total[roomNum].size(); i++){
						MsgInfo msg = holdback_total[roomNum].at(i);
						if(msg.time == propMsgTime && msg.message == propMsg){
							curtIdx = i;
							break;
						}
					}
					if(curtIdx != -1){
						// update proposal count and max proposal
//						if(verbalMode){
//							cout << getTime()<< "pCount ="<<holdback_total[roomNum].at(curtIdx).pCount<<
//									"pMax = "<<holdback_total[roomNum].at(curtIdx).pMax<<endl;
//						}
						holdback_total[roomNum].at(curtIdx).pCount += 1;
						holdback_total[roomNum].at(curtIdx).pMax = max(holdback_total[roomNum].at(curtIdx).pMax,proposeNum);
						if(verbalMode){
							cout << getTime()<< "pCount ="<<holdback_total[roomNum].at(curtIdx).pCount<<
									"pMax = "<<holdback_total[roomNum].at(curtIdx).pMax<<endl;
						}
						if(holdback_total[roomNum].at(curtIdx).pCount == totalServerNum){
							//collect all node's proposal, could make decision
							// decision data: ![room number],[decision],[message],[time]
							int decisionNum = holdback_total[roomNum].at(curtIdx).pMax;
							string decision = "!"+to_string(roomNum)+",";
							decision += to_string(decisionNum)+",";
							decision += holdback_total[roomNum].at(curtIdx).message + ",";
							decision += holdback_total[roomNum].at(curtIdx).time;
							//multicast the decision to all other server
							if(verbalMode){
								cout << getTime()<< "Collect all proposals, decision = "
										<<holdback_total[roomNum].at(curtIdx).pMax<<" for message"<<endl;
							}
							holdback_total[roomNum][curtIdx].seqNum = decisionNum;//update m's sequence number to Tm
							holdback_total[roomNum][curtIdx].isDeliverable = true;//set the state to deliverable
							//update A[g]
							A[roomNum] = max(A[roomNum],decisionNum);
							//check if all msg in holdback are deliverable
							bool allDeliver = true;
							for(int i=0; i < holdback_total[roomNum].size();i++){
								MsgInfo msg = holdback_total[roomNum].at(i);
								if(!msg.isDeliverable) {
									allDeliver = false;
									break;
								}
							}

							sort(holdback_total[roomNum].begin(),holdback_total[roomNum].end(),Comp() );
							// deliver the message in the holdback queue with the min sequence number
							// once the message is deliverable
							int index = 0;
							MsgInfo toDeliver = holdback_total[roomNum][index];

							while(index < holdback_total[roomNum].size() && toDeliver.isDeliverable){
								//toDeliver is currently the message with min seqNum, deliver message
								deliverMessage(sock,roomNum,toDeliver.message);
								//update toDeliver
								if(verbalMode){
									cout <<getTime()<< "Curt Server deliver message"<<toDeliver.message<<endl;
								}
								toDeliver = holdback_total[roomNum][++index];
							}
							//After deliver all available message
							//remove delivered message from holdback queue
							if(index != 0){
								holdback_total[roomNum].erase(holdback_total[roomNum].begin(),
												holdback_total[roomNum].begin() + index);
							}

							multicastMsg(sock,decision);
						}
					}else{
						cerr<<"-ERR In Proposal, could not find message in holdback queue"<<endl;
					}
				}else if(data[0] == '!'){//decision: ![room number],[decision],[message],[time]
					int roomNum = atoi(data.substr(1,data.find(',')).c_str());
					if(roomNum <= 0 || roomNum > MAX_NUM_ROOM ){
							cerr << getTime() << "room number invalid"<<endl;
							continue;
					}
					string restStr = data.substr(data.find(',')+1);
					int decisionNum = atoi(restStr.substr(0,restStr.find(',')).c_str());
					if(decisionNum <= 0){
						cerr << getTime() << "-ERR invalid decision number"<<endl;
					}
					// server receives other server's proposal
					if(verbalMode){
						cout << getTime()<< "Receive server decision message: "<<data<<endl;
					}
					// find message from vector
					int curtIdx = -1;
					string rest2 = restStr.substr(restStr.find(',')+1);
					string decMsg = rest2.substr(0,rest2.find(','));
					string decMsgTime = rest2.substr(rest2.find(',')+1);
					for(int i = 0; i < holdback_total[roomNum].size(); i++){
						MsgInfo msg = holdback_total[roomNum][i];
						if(msg.time == decMsgTime && msg.message == decMsg){
							curtIdx = i;
							break;
						}
					}
					if(curtIdx != -1){
						holdback_total[roomNum][curtIdx].seqNum = decisionNum;//update m's sequence number to Tm
						holdback_total[roomNum][curtIdx].isDeliverable = true;//set the state to deliverable
						//update A[g]
						A[roomNum] = max(A[roomNum],decisionNum);
						// Store the deliverable message to a priority queue and deliver message
						// from small sequence to large sequence number
						sort(holdback_total[roomNum].begin(),holdback_total[roomNum].end(),Comp() );
						// deliver the message in the holdback queue with the min sequence number
						// once the message is deliverable
						int index = 0;
						MsgInfo toDeliver = holdback_total[roomNum][index];

						while(index < holdback_total[roomNum].size() && toDeliver.isDeliverable){
							//toDeliver is currently the message with min seqNum, deliver message
							deliverMessage(sock,roomNum,toDeliver.message);
							//update toDeliver
							if(verbalMode){
								cout << "deliver message"<<toDeliver.message<<endl;
							}
							toDeliver = holdback_total[roomNum][++index];
						}
						//After deliver all available message
						//remove delivered message from holdback queue
						if(index != 0){
							holdback_total[roomNum].erase(holdback_total[roomNum].begin(),
											holdback_total[roomNum].begin() + index);
						}

					}else{
						cerr<<"-ERR In Decision, could not find message in holdback queue"<<endl;
					}

				}else{//message data = [roomNum],[msg],[time]
					// get room number
					int roomNum = atoi(data.substr(0,data.find(',')).c_str());
					if(roomNum <= 0 || roomNum > MAX_NUM_ROOM ){
						cerr << getTime() << "room number invalid"<<endl;
						continue;
					}
					// server receives other server's request
					if(verbalMode){
						cout << getTime()<< "Receive TOTAL server multicast message "<<data<<endl;
					}
					string msgTime = data.substr(data.find(',')+1);
					// propose a sequence number to the senderServer
					// update the proposed number
					P[roomNum] = max(P[roomNum],A[roomNum]) + 1;
					//proposal data: ?[room number],[proposal],[message]
					string proposal = "?"+to_string(roomNum)+",";
					proposal += to_string(P[roomNum])+","+msgTime;
					// send proposal back to sender server
					sendMsg(sock,src,proposal);
					if(verbalMode){
						cout << getTime()<< "Made proposal: "<<proposal<<endl;
					}
					//store the message to holdback and mark as undeliverable
					struct MsgInfo proMsg;
					proMsg.isDeliverable = false;
					proMsg.message = msgTime.substr(0,msgTime.find(','));
					proMsg.seqNum = P[roomNum];
					proMsg.serverID = senderIdx;
					proMsg.time = msgTime.substr(msgTime.find(',')+1);
					holdback_total[roomNum].push_back(proMsg);
				}


			}
		}
		else{//message from client
			//get client name
			string clientName;
			cout <<"***********In client**************"<<endl;
			cout << "Recieve message: "<<data << " from "<<senderKey <<endl;
			if(nickname.find(senderKey) == nickname.end()){
				clientName = senderKey;
			}else{
				clientName = nickname[senderKey];
			}
			if(data.substr(0,5).compare("/join") == 0){
				if(data.length() <= 6){
					sendMsg(sock,src,"-ERR the number of room need to specified");
					continue;
				}
				int roomNum = atoi(data.substr(6).c_str());
				if(roomNum < 1 || roomNum > MAX_NUM_ROOM){
					sendMsg(sock,src,"-ERR the specified chat room not exists(valid room: 1 to "+to_string(MAX_NUM_ROOM)+")");
					continue;
				}else{//check if user already in one room
					if(clientRoomMap.find(senderKey) != clientRoomMap.end()){
						//user already in a chat room
						sendMsg(sock,src,"-ERR You are already in room #"+to_string(clientRoomMap[senderKey]));
						continue;
					}else{//add user to room
						clientRoomMap[senderKey] = roomNum;
						chatRooms[roomNum].insert(senderKey);
						sendMsg(sock,src,"+OK You are now in chat room #"+to_string(roomNum));
						if(verbalMode){
							cout<< getTime() << "client <"<<clientName <<">"<<" join room #" << to_string(roomNum)<<endl;
						}
					}
				}
			}else if(data.compare("/part") == 0){
				//check if user not currently in one room
				if(clientRoomMap.find(senderKey) == clientRoomMap.end()){
					//user already in a chat room
					sendMsg(sock,src,"-ERR You are not currently in one room");
					continue;
				}else{
					int roomNum = clientRoomMap[senderKey];
					// remove user from room
					clientRoomMap.erase(senderKey);
					chatRooms[roomNum].erase(senderKey);
					sendMsg(sock,src,"+OK You have left chat room #"+to_string(roomNum));
					if(verbalMode){
						cout<< getTime() << "client <"<<clientName <<">"<<" left room #" << to_string(roomNum)<<endl;
					}
				}
			}else if(data.substr(0,5).compare("/nick") == 0){
				string nickName;
				if(data.length() <= 6){
					nickName = senderKey;
				}else{
					nickName = data.substr(6);
				}//store the nickName into nickName map
				nickname[senderKey] = nickName;
				sendMsg(sock,src,"+OK Nickname set to \'"+nickName+"\'");
				if(verbalMode){
					cout<< getTime() << "client <"<<clientName <<">"<<" change nickname to \'" << nickName<<"\'"<<endl;
				}
			}else if(data.compare("/quit")==0){
				//remove the chat client from room,active client
				if(clientRoomMap.find(senderKey) != clientRoomMap.end()){
					int curtRoomNum = clientRoomMap[senderKey];
					chatRooms[curtRoomNum].erase(senderKey);
					clientRoomMap.erase(senderKey);
				}
				if(nickname.find(senderKey) != nickname.end()){
					nickname.erase(senderKey);
				}
				if(verbalMode){
					cout<< getTime() << "client <"<<clientName <<">"<<" removed from active client list"<<endl;
				}
			}else{//client send message to server
				if(data.size() == 0) continue;
				//check if client has joined a room
				if(clientRoomMap.find(senderKey) != clientRoomMap.end()){
					if(verbalMode){
						cout<<getTime()<< "Receive message" <<data<<" from client"<<clientName<<endl;
					}

					int roomNum = clientRoomMap[senderKey];

					if(order == UNORDER){
						deliverMessage(sock,roomNum,"<"+clientName + "> "+data);
						//append groupNum,<client name> before message
						string response = to_string(roomNum) + ",";
						response += "<"+clientName + "> ";
						response += data;
						multicastMsg(sock,response);
					}else if(order == FIFO){
						//deliver message to client in the same room
						deliverMessage(sock,roomNum,"<"+clientName + "> "+data);
						//multicast message to server message format [roomNum],[msgID],[message]
						int msgID = ++S[roomNum];
						string response = to_string(roomNum) + ",";
						response += to_string(msgID) + ",";
						response += "<"+clientName + "> ";
						response += data;
						multicastMsg(sock,response);
					}else{
						// For Total ordering
						// N invokes B-multicast [roomNum],[<clientName> msg],[time]
						string response = to_string(roomNum)+",";
						string timeReceive = getTime();
						response += "<"+clientName + "> "+data+","+timeReceive;

						// send message to sender client
//						sendMsg(sock,src,"<"+clientName + "> "+data);
						// make a proposal for itself and store the message into holdback queue
						P[roomNum] = max(P[roomNum],A[roomNum]) + 1; //update propose number
						if(verbalMode){
							cout<<"Self proposal for msg:"<<P[roomNum]<<endl;
						}
						struct MsgInfo curtMsgInfo;
						curtMsgInfo.isDeliverable = false;
						curtMsgInfo.message = "<"+clientName + "> "+data;
						curtMsgInfo.seqNum = P[roomNum];
						curtMsgInfo.serverID = curtServerIdx;
						curtMsgInfo.time = timeReceive;
						curtMsgInfo.pCount = 1; //now only itself propose
						curtMsgInfo.pMax = P[roomNum];
						//then put (m, Pg,new) into their local hold-back queue marked as undeliverable
						holdback_total[roomNum].push_back(curtMsgInfo);

						multicastMsg(sock,response);//multicast to all other server to ask for proposal
					}
				}else{
					sendMsg(sock,src,"-ERR client not in a room yet");
				}
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
//		cout<<string(inet_ntoa(dest.sin_addr)) << to_string(ntohs(dest.sin_port))<<endl;
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
//			cout<<string(inet_ntoa(dest.sin_addr)) << to_string(ntohs(dest.sin_port)) <<endl;
		}
	}
}


