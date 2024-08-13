#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <time.h>
#include <semaphore.h>
#include <math.h>

#define MAX_EVENTS 30
#define FD 5          // Linux는 connectSd를 5부터 할당
#define DIGITS 4

// 게임-방 구조체
typedef struct gameInfo {
	int host;
	int playerCnt;
} gI;

// 게임 데이터 구조체
typedef struct gameData {
	int goal;
	int round;
} gD;

// 플레이어 구조체
struct player {
	int fd;
	int score;
}
;
// 결과 데이터 구조체
typedef struct gameResult {
	struct player participant[7];
	int winner[10];
	int next_host;
} gR;

// thread 전달 구조체
typedef struct passStruct {
	int listenSd;
	gI* gmInfo;
	gD* gmData;
	struct player* player;
	gR* gmRes;
} pS;

// 함수 선언부
void errProc(const char*);
void stSet(gI*, gD*, struct player*, gR*);
int findIdx(int, gI*, gR*);
int srchFDList(int);
void msgSendAll(int);
void msgSendExcl(int, int);
void gmWinner(gI*, gR*);
void printRes(gI*, gD*, gR*);
void cleanUp(gI*, gD*, struct player*, gR*);
void tmFormatter(char*);
void* gmCtrl(void*);
void* sleepTM(void*);
int countStrike(int, int);
int countBall(int, int);
int new_target(void);

// 전역 변수
int deFDList[MAX_EVENTS] = { 0, }; // 사용하지 않는 파일 디스크립터 배열
time_t timer;					   // 타이머
char sBuff[BUFSIZ];			       // 전송 버퍼
pthread_t thread_sleep;			   // 타이머 thread
int eventCnt = 0;				   // 카운터
int inGameFlag = 0;				   // 인게임 flag
int conFlag = 1;				   // 생성자 flag
int chance;						   // 정답 제출 기회
sem_t semaphore;				   // 세마포어

// epoll 관련 변수
int epfd;
struct epoll_event event;
struct epoll_event events[MAX_EVENTS];

int main(int argc, char* argv[])
{
	// 변수 선언 
	int listenSd, connectSd, readFd;
	struct sockaddr_in srvAddr, clntAddr;
	int clntAddrLen, reCnt, nRecv;
	int i, j, k;
	char buff[BUFSIZ];
	char tBuff[30];
	int breakFlag = 1;
	int target;					 // 정답
	int guess;					 // 예측 숫자
	int cntStrike;				 // 스트라이크 개수
	int cntBall;				 // 볼 개수
	int deCnt = 0;				 // 카운터
	pthread_t thread_game;		 // 게임 컨트롤 thread
	sem_init(&semaphore, 0, 1);  // 세마포어 생성

	gI* gmInfo = malloc(sizeof(gI)); // 게임-방 구조체
	gD* gmData = malloc(sizeof(gD)); // 게임 데이터 구조체
	struct player player;			 // 플레이어 구조체
	gR* gmRes = malloc(sizeof(gR));  // 결과 데이터 구조체

	// 구조체 초기화
	stSet(gmInfo, gmData, &player, gmRes);
	struct passStruct* passStruct = malloc(sizeof(struct passStruct));
	passStruct->listenSd = listenSd;
	passStruct->gmInfo = gmInfo;
	passStruct->gmData = gmData;
	passStruct->player = &player;
	passStruct->gmRes = gmRes;

	// 예외 처리
	if (argc != 2) {
		printf("Usage: %s [Port Number]\n", argv[0]);
		exit(1);
	}
	printf("\n\n\n==================== Server Program ====================\n\n");

	// 소켓 생성
	listenSd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSd == -1) errProc("socket");

	// 소켓 주소 구조체 초기화
	memset(&srvAddr, 0, sizeof(srvAddr));
	srvAddr.sin_family = AF_INET;
	srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	srvAddr.sin_port = htons(atoi(argv[1]));

	// 소켓 Bind
	if (bind(listenSd, (struct sockaddr*)&srvAddr, sizeof(srvAddr)) == -1)
		errProc("bind");

	// listen 상태 전환
	if (listen(listenSd, 10) < 0) // Backlog = 10
		errProc("listen");

	// epoll 객체 생성
	epfd = epoll_create(1); // 인자 생략가능
	if (epfd == -1) errProc("epoll_create");

	// epoll 객체에 관심있는 listenSd 추가
	event.events = EPOLLIN;    // 이벤트 종류
	event.data.fd = listenSd;  // 파일 디스크립터
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenSd, &event) == -1)
		errProc("epoll_ctl");

	// 반복문을 활용하여 모니터링
	clntAddrLen = sizeof(clntAddr);
	while (breakFlag) {
		// Level-Triggered 관찰
		reCnt = epoll_wait(epfd, events, MAX_EVENTS, -1); // time 제한없음
		if (reCnt == -1) {
			if (errno == EINTR) continue; // 시스템 콜 중 인터럽트
			else errProc("epoll_wait");
		}

		// 이벤트가 관찰된(I/O가 필요한)만큼 반복
		for (i = 0; i < reCnt; i++) {
			// listenSd에 해당하는 이벤트
			if (events[i].data.fd == listenSd) {
				// 클라이언트 accept
				connectSd = accept(listenSd,
					(struct sockaddr*)&clntAddr, &clntAddrLen);
				if (connectSd == -1) {
					fprintf(stderr, "Accept Error\n");
					continue;
				}

				// 새로운 파일 디스크립터를 epoll 객체에 추가
				event.data.fd = connectSd;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, connectSd, &event) == -1)
					errProc("epoll_ctl");

				// 최초 접속자 및 방장에게 게임 시작 조건 알림
				if (connectSd == gmInfo->host) {
					sprintf(sBuff,
						"<정답 채팅> : 중복되는 수가 없는 4자리 정수.\n[게임 시작] -> 정답 채팅 입력");
					send(connectSd, sBuff, sizeof(sBuff) - 1, 0);
				}
				// 정답 채팅에 대한 설명
				else {
					sprintf(sBuff, "<정답 채팅> : 중복되는 수가 없는 4자리 정수.");
					send(connectSd, sBuff, sizeof(sBuff) - 1, 0);
				}

				// 클라이언트들에게 입장을 알림
				sprintf(sBuff, "Player%d 입장 - (현재 인원: %d)", connectSd - 4, eventCnt - deCnt + 1);
				msgSendAll(eventCnt);

				eventCnt++;
				// 화면에 출력 및 카운터 증가
				printf("Player%d 입장 - (현재 인원: %d).\n\n", eventCnt, eventCnt - deCnt);
			}
			// connectSd에 해당하는 이벤트, I/O
			else {
				// 클라이언트로부터 메시지 수신 
				readFd = events[i].data.fd;
				nRecv = recv(readFd, buff, sizeof(buff) - 1, 0);

				// 클라이언트의 채팅방 퇴장
				if (nRecv == 0) {
					fprintf(stderr, "Player%d의 연결이 끊어짐.\n\n", readFd - 4);
					// epoll 객체에서 파일 디스크립터 삭제
					if (epoll_ctl(epfd, EPOLL_CTL_DEL, readFd, &event) == -1)
						errProc("epoll_ctl");
					// 단, readFd를 닫지 않아야 새로운 참가자를 구분할 수 있다!
					// 사용하지 않는 파일 디스크립터 등록
					deFDList[deCnt++] = readFd;

					// 퇴장 메시지를 다른 클라이언트들에게 전송
					sprintf(sBuff, "Player%d 퇴장 - (현재 인원: %d)", readFd - 4, eventCnt - deCnt);
					// 자기 자신은 제외
					msgSendExcl(readFd, eventCnt);

					// 게임 관련 구조체 변경
					k = findIdx(readFd, gmInfo, gmRes);
					sem_wait(&semaphore);
					for (j = k + 1; j < gmInfo->playerCnt; j++)
						(gmRes->participant)[j - 1] = (gmRes->participant)[j];
					(gmInfo->playerCnt)--;
					sem_post(&semaphore);

					// 모두 퇴장시 새로운 플레이어에게 방장 부여
					if (eventCnt == deCnt) {
						gmInfo->host = FD + eventCnt;
						continue;
					}

					// 게임이 진행중일 때 퇴장
					if (inGameFlag) {
						// 플레이어가 2명 미만일 시 게임 종료
						if (gmInfo->playerCnt < 2) {
							// thread 종료
							pthread_cancel(thread_sleep);
							pthread_cancel(thread_game);

							// 게임 종료 메시지 출력 및 전송
							printf("게임 종료~\n\n");
							sprintf(sBuff, "게임 종료~");
							msgSendExcl(readFd, eventCnt);

							// clean-up 함수 호출
							cleanUp(gmInfo, gmData, &player, gmRes);
						}
						else {
							// 방장 퇴장시
							// 서버에서 임의의 숫자를 제공해 이번 라운드 새로 시작
							if (readFd == gmInfo->host) {
								pthread_cancel(thread_game);
								pthread_create(&thread_game, NULL, gmCtrl, (void*)passStruct);
								target = new_target();
								gmData->goal = target;
								printf("현재 라운드 재시작!\n새로운 정답 : [%d]\n\n", target);
								sprintf(sBuff, "현재 라운드 재시작!");
								msgSendAll(eventCnt);
							}

							// 타이머 작동 중인 클라이언트 퇴장
							if (readFd == player.fd) {
								pthread_cancel(thread_sleep);
								chance++;
							}
						}
					}
					// 게임이 진행중이지 않을 때 퇴장
					else {
						// 게임이 존재할 때   
						if (!conFlag) {
							// 플레이어가 2명 미만일 시 게임 종료
							if (gmInfo->playerCnt < 2) {
								printf("게임 종료~\n\n");
								sprintf(sBuff, "게임 종료~");
								msgSendExcl(readFd, eventCnt);
								cleanUp(gmInfo, gmData, &player, gmRes);
								break;
							}
						}

						// 방장 퇴장 시
						if (readFd == gmInfo->host) {
							// 남은 플레이어 중 가장 먼저 입장한 사람에게 방장 부여
							j = FD;
							while (1) {
								if (!srchFDList(j)) break;
								j++;
							}

							gmInfo->host = j;
							sprintf(sBuff, "[게임 시작] -> 정답 채팅 입력");
							send(gmInfo->host, sBuff, sizeof(sBuff) - 1, 0);
						}
					}

					continue;
				}

				// 수신한 메시지 출력
				buff[nRecv] = '\0';
				if (readFd == gmInfo->host) {
					printf("From Client : Player%d(방장) : %s\n\n", readFd - 4, buff);

					// 새로운 게임 시작을 담당하는 방장
					if (!inGameFlag && conFlag) {
						// 방장의 요청에 의해 게임 시작
						if (0 < atoi(buff) && atoi(buff) <= 9999 && (eventCnt - deCnt) >= 2) {
							// 숫자야구 정답 저장
							target = atoi(buff);
							gmData->goal = target;
							gmInfo->playerCnt = eventCnt - deCnt; // 플레이어 수 저장

							// 클라이언트들에게 메시지 전송 및 플레이어 파악
							printf("게임 시작! (플레이어: %d명)\n\n", gmInfo->playerCnt);
							sprintf(sBuff, "게임 시작! (플레이어: %d명)", gmInfo->playerCnt);
							msgSendAll(eventCnt);
							k = 0;
							for (j = 0; j < eventCnt; j++) {
								if (srchFDList(FD + j)) continue;
								player.fd = FD + j;
								(gmRes->participant)[k++] = player; // 플레이어 정보 저장
							}

							// 게임 컨트롤 thread 생성
							pthread_create(&thread_game, NULL, gmCtrl, (void*)passStruct);

							// Flag 변경
							inGameFlag = 1;
							conFlag = 0;
							continue;
						}
						// 게임 시작 조건이 충족되지 않았을 때
						else if (0 < atoi(buff) && atoi(buff) <= 9999) {
							// 예외 메시지 전송
							sprintf(sBuff, "인원이 충분하지 않습니다!");
							send(readFd, sBuff, sizeof(sBuff) - 1, 0);
							continue;
						}
					}
					// 새로운 라운드 시작을 담당하는 방장
					else if (!inGameFlag) {
						if (0 < atoi(buff) && atoi(buff) <= 9999 && (eventCnt - deCnt) >= 2) {
							// 숫자야구 정답 저장
							target = atoi(buff);
							gmData->goal = target;

							// n 라운드 시작 메시지 전체 전송
							printf("%2d Round 시작! (플레이어: %d명)\n\n",
								gmData->round + 1, gmInfo->playerCnt);
							sprintf(sBuff, "%2d Round 시작! (플레이어: %d명)",
								gmData->round + 1, gmInfo->playerCnt);
							msgSendAll(eventCnt);

							// 게임 컨트롤 thread 생성
							pthread_create(&thread_game, NULL, gmCtrl, (void*)passStruct);

							// Flag 변경
							inGameFlag = 1;
							continue;
						}
						// 게임 시작 조건이 충족되지 않았을 때
						else if (0 < atoi(buff) && atoi(buff) <= 9999) {
							// 예외 메시지 전송
							sprintf(sBuff, "인원이 충분하지 않습니다!");
							send(readFd, sBuff, sizeof(sBuff) - 1, 0);
							continue;
						}
					}
				}
				else {
					printf("From Client : Player%d : %s\n\n", readFd - 4, buff);

					// 게임중일 때 분기
					if (inGameFlag) {
						// 4자리 수의 입력 -> 정답 제출로 간주 
						if (readFd == player.fd && 0 < atoi(buff) && atoi(buff) <= 9999) {
							// 제출에 대한 답변 알고리즘
							// 예측 숫자 저장
							guess = atoi(buff);
							cntStrike = countStrike(guess, target);
							cntBall = countBall(guess, target);

							// 정답을 제출했을 때 분기
							if (cntStrike == 4) { 
								// thread 종료
								pthread_cancel(thread_sleep);
								pthread_cancel(thread_game);

								// 정답 알림 메시지 전송
								printf("Player%d 승리!\n\n", readFd - 4);
								sprintf(sBuff, "Player%d 승리!", readFd - 4);
								msgSendAll(eventCnt);

								// 정답자 기록 및 점수 갱신
								k = findIdx(readFd, gmInfo, gmRes);
								(gmRes->participant)[k].score += 3;
								(gmRes->winner)[gmData->round++] = (gmRes->participant)[k].fd;

								// 모든 라운드가 종료되었을 시
								if (gmData->round == 10) {
									// 게임 종료 메시지 출력 및 전송
									printf("게임 종료~\n\n");
									sprintf(sBuff, "게임 종료~");
									msgSendAll(eventCnt);

									// clean_up 함수 호출
									cleanUp(gmInfo, gmData, &player, gmRes);
									continue;
								}

								// 방장 설정 후 게임 시작 조건 알림
								gmInfo->host = readFd;
								sprintf(sBuff, "[게임 시작] -> 정답 채팅 입력!");
								send(gmInfo->host, sBuff, sizeof(sBuff) - 1, 0);

								// Flag 변경
								inGameFlag = 0;
							}
							// 정답 아닐 시
							else {
								pthread_cancel(thread_sleep);
								printf("[결과] -> %d Strike, %d Ball\n\n", cntStrike, cntBall);
								sprintf(sBuff, "[결과] -> %d Strike, %d Ball", cntStrike, cntBall);
								msgSendAll(eventCnt);
							}

							continue;
						}
					}
				}

				// 수신 메시지 변환
				tmFormatter(tBuff); // 타이머 + 형식변환
				if (readFd == gmInfo->host)
					sprintf(sBuff, "[%s] Player%d(방장) : %s", tBuff, readFd - 4, buff);
				else
					sprintf(sBuff, "[%s] Player%d : %s", tBuff, readFd - 4, buff);

				// 클라이언트들에게 메시지 전송
				msgSendExcl(readFd, eventCnt);
			}
		}
	}

	// 파일 디스크립터 닫기
	for (j = 0; j < eventCnt; j++)
		close(FD + j);
	close(listenSd);
	close(epfd);
	sem_destroy(&semaphore);
	
	return 0;
}

// 에러 출력 함수
void errProc(const char* str) {
	fprintf(stderr, "%s: %s\n", str, strerror(errno));
	exit(1); // 비정상 종료
}

// 구조체 초기화 함수
void stSet(gI* gmInfo, gD* gmData, struct player* player, gR* gmRes) {
	memset(gmInfo, 0, sizeof(struct gameInfo));
	gmInfo->host = FD;
	memset(gmData, 0, sizeof(struct gameData));
	memset(player, 0, sizeof(struct player));
	memset(gmRes, 0, sizeof(struct gameResult));
}

// 구조체 인덱스 반환 함수
int findIdx(int readFd, gI* gmInfo, gR* gmRes) {
	// 지역 변수
	int idx, j;

	// 구조체 탐색
	for (j = 0; j < gmInfo->playerCnt; j++) {
		if ((gmRes->participant)[j].fd == readFd) {
			idx = j;
			break;
		}
	}

	return idx;
}

// 미사용 파일 디스크립터 검색 함수
int srchFDList(int value) {
	// 지역 변수
	int i;

	// 배열 탐색
	for (i = 0; i < MAX_EVENTS; i++) {
		if (deFDList[i] == value)
			return 1;
	}

	return 0;
}

// 메시지 전달 함수
void msgSendAll(int eventCnt) {
	// 지역 변수
	int i;

	// 메시지 전송
	for (i = 0; i < eventCnt; i++) {
		if (srchFDList(FD + i)) continue;
		send(FD + i, sBuff, sizeof(sBuff) - 1, 0);
	}
}
void msgSendExcl(int readFd, int eventCnt) {
	// 지역 변수
	int i;

	// 메시지 전송
	for (i = 0; i < eventCnt; i++) {
		if ((FD + i) == readFd || srchFDList(FD + i))
			continue;
		send(FD + i, sBuff, sizeof(sBuff) - 1, 0);
	}
}

// 승리자 탐색 함수
void gmWinner(gI* gmInfo, gR* gmRes) {
	// 지역 변수
	int i, idx = 0;
	int max = -1;

	// 결과 데이터 구조체 탐색
	for (i = 0; i < gmInfo->playerCnt; i++) {
		if (max < (gmRes->participant)[i].score) {
			max = (gmRes->participant)[i].score;
			idx = i;
		}
	}

	// 결과 데이터 구조체에 적용
	gmRes->next_host = (gmRes->participant)[idx].fd;
}

// 최종 결과 출력 함수
void printRes(gI* gmInfo, gD* gmData, gR* gmRes) {
	// 지역 변수
	int i;
	int winner;
	char tmp[100];

	// for-loop문
	// 출력할 문자열 생성
	i = 0;
	sBuff[0] = '\0';
	for (i = 0; i < gmData->round; i++) {
		winner = (gmRes->winner)[i];
		if (winner == -1)
			sprintf(tmp, "%2d Round - 정답자 없음\n", i + 1);
		else
			sprintf(tmp, "%2d Round 승리자 -> Player %d\n", i + 1, (gmRes->winner)[i] - 4);
		strcat(sBuff, tmp);
	}

	// 최종 점수 출력
	sprintf(tmp, "\n[최종 결과]\n");
	strcat(sBuff, tmp);
	for (i = 0; i < gmInfo->playerCnt; i++) {
		sprintf(tmp, "(Player%d, %d점)\n", (gmRes->participant)[i].fd - 4, (gmRes->participant)[i].score);
		strcat(sBuff, tmp);
	}

	gmWinner(gmInfo, gmRes); // 승리자 탐색 함수 호출
	sprintf(tmp, "------------------------------\n< 게임 승리자 : Player %d >", gmRes->next_host - 4);
	strcat(sBuff, tmp);

	// 출력 및 클라이언트들에게 메시지 전송
	printf("%s\n------------------------------\n\n", sBuff);
	msgSendAll(eventCnt);
}

// 게임 종료시 clean-up
void cleanUp(gI* gmInfo, gD* gmData, struct player* player, gR* gmRes) {
	// 지역 변수
	int tmp;

	// 최종 결과 출력
	printRes(gmInfo, gmData, gmRes);
	tmp = gmRes->next_host;

	// 변수 초기화
	stSet(gmInfo, gmData, player, gmRes);
	inGameFlag = 0;
	conFlag = 1;

	// 방장 설정후 게임 시작 조건 알림
	gmInfo->host = tmp;
	sprintf(sBuff, "[게임 시작] -> 정답 채팅 입력!");
	send(gmInfo->host, sBuff, sizeof(sBuff) - 1, 0);
}

// 타이머 형식변환 함수
void tmFormatter(char* tBuff) {
	// 현재 시간 반환
	time(&timer);
	// 버퍼에 복사
	strcpy(tBuff, ctime(&timer));
	// 개행 문자 지우기
	tBuff[strlen(tBuff) - 1] = '\0';
}

// 게임 컨트롤 thread
void* gmCtrl(void* arg) {
	// 지역 변수
	int tmp;
	int i, j;
	struct passStruct* passStruct = (struct passStruct*)arg;

	chance = 10;
	while (1) {
		// 탈출 조건
		if (chance == 0) break;

		// for-loop문
		// 각 플레이어에게 정답 타이머 부여
		for (i = 0; i < passStruct->gmInfo->playerCnt; i++) {
			sem_wait(&semaphore);
			tmp = passStruct->gmRes->participant[i].fd;
			sem_post(&semaphore);
			// 방장에게는 정답 기회를 제공하지 않음
			if (tmp == passStruct->gmInfo->host) continue;

			// 게임 참여자 전원에게 알림 메시지 전송
			printf("[정답 타이머 30초]\nPlayer %d의 차례 - (남은 기회: %d번)\n\n", tmp - 4, chance);
			sprintf(sBuff, "[정답 타이머 30초]\nPlayer %d의 차례 - (남은 기회: %d번)", tmp - 4, chance);
			msgSendAll(eventCnt);

			// 플레이어 구조체에 타이머가 부여된 플레이어 설정
			passStruct->player->fd = tmp;
			// 타이머 thread 생성
			pthread_create(&thread_sleep, NULL, sleepTM, NULL);
			// thread 종료 대기
			pthread_join(thread_sleep, NULL);
		}

		chance--; // 기회 차감
	}

	// 정답자 없이 라운드 종료시    
	// 결과 알림 메시지 전송
	printf("모든 기회를 사용했으므로, 다음 라운드로 넘어갑니다!\n\n");
	sprintf(sBuff, "모든 기회를 사용했으므로, 다음 라운드로 넘어갑니다!\n이번 라운드 정답 : [%d]", 
			passStruct->gmData->goal);
	msgSendAll(eventCnt);
	(passStruct->gmRes->winner)[passStruct->gmData->round++] = -1;

	// 모든 라운드가 종료되었을 시
	if (passStruct->gmData->round == 10) {
		// 게임 종료 메시지 출력 및 전송
		printf("게임 종료~\n\n");
		sprintf(sBuff, "게임 종료~");
		msgSendAll(eventCnt);

		// clean_up 함수 호출
		cleanUp(passStruct->gmInfo, passStruct->gmData,
			passStruct->player, passStruct->gmRes);
		return;
	}

	// 다음 라운드가 존재할 시  
	// 새로운 방장 설정
	for (i = 0; i < passStruct->gmInfo->playerCnt; i++) {
		tmp = passStruct->gmRes->participant[i].fd;
		if (tmp > passStruct->gmInfo->host) {
			passStruct->gmInfo->host = tmp;
			break;
		}
	}

	// 방장에게 게임 시작 조건 알림
	sprintf(sBuff, "[게임 시작] -> 정답 채팅 입력!");
	send(passStruct->gmInfo->host, sBuff, sizeof(sBuff) - 1, 0);

	// Flag 변경
	inGameFlag = 0;
}

// 타이머 thread
void* sleepTM(void* arg) {
	//  ðŭ sleep
	sleep(30);
}

// 스트라이크 수 반환 알고리즘
int countStrike(int guess, int target) {
	// 지역 변수
	int strike = 0;
	int targetArray[DIGITS] = { 0 };
	int guessArray[DIGITS] = { 0 };
	int i, j;
	int divisor;

	// 반복 돌면서 처리
	// 스트라이크 수 연산
	for (i = 0; i < DIGITS; i++) {
		divisor = 1;
		for (j = 0; j < 3 - i; j++) {
			divisor *= 10;
		}
		targetArray[i] = target / divisor;
		target -= (targetArray[i] * divisor);
		guessArray[i] = guess / divisor;
		guess -= (guessArray[i] * divisor);

		if (targetArray[i] == guessArray[i])
			strike++;
	}

	// 스트라이크 개수 반환
	return strike;
}

// 볼 수 반환 알고리즘
int countBall(int guess, int target) {
	// 지역 변수
	int ball = 0;
	int targetArray[DIGITS] = { 0 };
	int guessArray[DIGITS] = { 0 };
	int i, j;
	int divisor;

	// 반복 돌면서 처리
	for (i = 0; i < DIGITS; i++) {
		divisor = 1;
		for (j = 0; j < 3 - i; j++) {
			divisor *= 10;
		}
		targetArray[i] = target / divisor;
		target -= (targetArray[i] * divisor);
		guessArray[i] = guess / divisor;
		guess -= (guessArray[i] * divisor);
	}

	// 볼 수 연산
	for (i = 0; i < DIGITS; i++) {
		for (j = 0; j < DIGITS; j++) {
			if (i != j) {
				if (targetArray[i] == guessArray[j])
					ball++;
			}
		}
	}

	// 볼 개수 반환
	return ball;
}

// 랜덤 숫자 알고리즘
int new_target(void) {
	// 지역 변수
	int tmp[4] = { 0, };
	int i, j;
	int random;
	int flag;
	int result = 0;
	srand(time(NULL));

	// 반복 돌면서 새 숫자 생성
	for (i = 0; i < 4; i++) {
		flag = 1;
		random = rand() % 10;

		// 첫 번째 수는 검사 X
		if (i == 0) {
			tmp[i] = random;
			continue;
		}

		// 중복 검사
		for (j = 0; j < i; j++) {
			if (tmp[j] == random) {
				flag = 0;
				break;
			}
		}

		// 검사 결과 분기
		if (flag)
			tmp[i] = random;
		else
			i--;
	}

	// 반복 돌면서 계산
	for (i = 3; i > -1; i--)
		result += tmp[i] * pow(10, 3 - i);

	return result;
}

