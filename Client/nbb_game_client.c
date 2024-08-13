#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>

// 에러 출력 함수 정의
void errProc(const char* msg) {
     fprintf(stderr, "%s: %s\n", msg, strerror(errno));
     exit(1); //비정상 종료
}

// 채팅 수신 thread
void* read_thread(void* arg) {
	// 지역 변수
	int clntSd = *((int*)arg);
	int nRecv;
	char rBuff[BUFSIZ];

	// while-loop에서 수신 대기
	while(1) {
		// 메시지 receive
		nRecv = recv(clntSd, rBuff, sizeof(rBuff)-1, 0);
		if(nRecv == -1) errProc("recv");
		rBuff[nRecv] = '\0';
		printf("\n%s\n", rBuff); // 화면에 출력
	}
}

// 답변 유효성 검사 메서드
int check_target(int target) { 	
	// 지역 변수
	int digits[10] = {0}; // 0부터 9까지의 숫자 개수를 저장하는 배열
	int num = target;
	int i;

    // 입력된 정답의 각 자릿수 숫자 개수를 계산
    while (num > 0) {
        int digit = num % 10;
        digits[digit]++;
        num /= 10;
    }

    // 유효성 검사: 숫자가 중복되는 경우
    for (i = 0; i < 10; i++) {
        if (digits[i] > 1) {
			printf("4자리 숫자에 중복된 수가 있습니다.\n다시 입력!\n\n");
			return 1;
        }
    }
    
	// 숫자가 중복되지 않은 경우
	return 0;
}

int main(int argc, char* argv[]) 
{
    // 변수 선언
    int clntSd;
	struct sockaddr_in clntAddr;
	size_t clntAddrLen;
	char buff[BUFSIZ];
    int inputLen, nSent;
	pthread_t thread_id;
	int res;
	int flag;

    // 예외처리
    if(argc != 3) {
		printf("Usage: %s [IP Address] [Port]\n", argv[0]);
		exit(1);
    }

    // 소켓 생성
    clntSd = socket(AF_INET, SOCK_STREAM, 0);
    if(clntSd == -1) errProc("socket");
    printf("\n\n\n==================== Client Program ====================\n\n");

    // 소켓 주소 구조체 초기화
    memset(&clntAddr, 0, sizeof(clntAddr));
    clntAddr.sin_family = AF_INET;
    clntAddr.sin_addr.s_addr = inet_addr(argv[1]);
    clntAddr.sin_port = htons(atoi(argv[2]));

    // connect 요청
    if(connect(clntSd, (struct sockaddr*)&clntAddr, sizeof(clntAddr)) == -1) {
		close(clntSd);
		errProc("connect");
    }
	printf("숫자야구게임 서버에 연결되었습니다.\n");

    // 채팅 알고리즘
	// 채팅환경 구현을 위해 thread 생성
	pthread_create(&thread_id, NULL, read_thread, &clntSd);
    while(1) {
		// 키보드로 입력받기
		fgets(buff, BUFSIZ, stdin);       //입력바이트 제한
		inputLen = strlen(buff) - 1;
		if(inputLen < 1) continue;  	  //입력값 없을 때
		if(!strcmp(buff, "END\n")) break; //종료 조건

		// 4자리 정수인지 검사
		// 게임 규칙 -> 1 ~ 9999는 특수 채팅으로 구분
		if(0 < atoi(buff) && atoi(buff) <= 999) {
			printf("4자리 정수를 입력해야 합니다.\n다시 입력!\n\n");
			continue;
		}
		// 중복된 숫자가 있는지 검사
		else if(1000 <= atoi(buff) && atoi(buff) <= 9999) {
			if(check_target(atoi(buff))) 
				continue;
		}

		// 서버로 전송
		nSent = send(clntSd, buff, inputLen, 0);
		if(nSent == -1) errProc("send");
    }

	// 탈출 시 thread 종료
	pthread_cancel(thread_id);
	printf("서버와의 접속 종료!\n프로그램을 종료합니다.\n");
    
    // 소켓 닫기
    close(clntSd);

    return 0;
}

