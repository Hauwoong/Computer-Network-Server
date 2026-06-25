//==========================================================
// server.cpp - 최소 동작 서버
// 연결을 1번 받아서, 받은 요청을 화면에 출력하고,
// 간단한 200 OK 응답을 보낸 뒤 종료한다.
//==========================================================

#include <iostream> // 화면 출력 cout
#include <string> // std::string 문자열
#include <sstream> // istringstream
#include <winsock2.h> // 윈도우 소켓 함수들 (socket, bind, ...)
#include <ws2tcpip.h> // 주소 관련 보조 함수
#include <windows.h> // SetConsoleOutputCP 사용 (winsock2.h 다음에 include!)

#pragma comment(lib, "ws2_32.lib") // ws2_32 라이브러리를 같이 링크 (MSVC용 표시)

using namespace std;

int main() 
{
    SetConsoleOutputCP(CP_UTF8); // 콘솔 출력을 UTF-8로 

    // ---- 1) WSAStartup: 윈도우 네트워크 사용 시작 ----
    WSADATA wsa;    // 윈도우 소켓 정보가 담길 구조체
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) // 버전 2.2로 초기화 요청
    {
        cout << "WSAStartup 실패\n";
        return 1;   // 실패하면 종료
    }

    // ---- 2) socket: 소켓(전화기) 만들기 ----
    // AF_INET = IPv4 주소체계, SOCK_STREAM = TCP(연결형)
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) 
    {
        cout << "socket 생성 실패\n";
        WSACleanup();
        return 1;
    }

    // ---- 3) bind: 소켓에 IP/포트 묶기 ----
    sockaddr_in addr;   // 주소 정보를 담는 구조체
    addr.sin_family = AF_INET;  // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;  // 이 PC의 모든 IP에서 접속 허용
    addr.sin_port = htons(8080);    // 포트 8080 (htons = 바이트 순서 변환)
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        cout << "bind 실패 (포트 사용주일 수 있음)\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    // ---- 4) listen: 손님(연결) 받을 준비 ----
    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
    {
        cout << "listen 실패\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }
    cout << "서버 시작! 포트 8080에서 연결 대기중...\n";

    // ---- 5) 반복: 연결을 계속 받는다 ----
    while(1) {
        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) continue;

        // ----6) recv: 클라이언트가 보낸 요청 글자 받기 ----
        char buffer[4096] = {0};    // 받은 글자를 담을 상자
        int len = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        if (len <= 0) {
            closesocket(clientSock); 
            continue;
        }
        buffer[len] = '\0';
        string requestStr(buffer);

        // --- 7) 파싱: 첫 줄에서 메서드/경로/버전 뽑기 ---
        istringstream iss(requestStr);
        string method, path, version;
        iss >> method >> path >> version;

         std::cout << "\n[요청 도착] 메서드=" << method << "  경로=" << path << "  버전=" << version << "\n";
    
        // --- 8) 응답 (아직은 무조건 200, Lesson 6에서 분기 예정) ---
        string body = "You requested: " + method + " " + path;
        string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + to_string(body.size()) + "\r\n"
            "\r\n" + body; //해더 끝 빈줄
        send(clientSock, response.c_str(), (int)response.size(), 0);

        // --- 9) 이 연결만 닫는다 (서버는 계속 살아있음) ---
        closesocket(clientSock);
    }


    
    closesocket(listenSock);
    WSACleanup();
    return 0;
}