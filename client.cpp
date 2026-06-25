// ==================================================================
//  client.cpp - Lesson 4: 최소 동작 클라이언트
//  서버(127.0.0.1:8080)에 연결해서 GET 요청을 보내고,
//  서버의 응답을 받아 출력한다.
// ==================================================================

#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ---- 1) WSAStartup ----
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cout << "WSAStartup 실패\n";
        return 1;
    }

    // --- 2) socket ----
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
    {
        cout << "socket 생성 실패\n";
        WSACleanup();
        return 1;
    }

    // ---- 3) 서버 주소 준비 + connect ----
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "connect 실패 (서버가 켜져 있나요?)\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    cout << "서버에 연결됨!\n";

    // ---- 4) 요청 (Request) 글자 만들어 보내기 ----
    //  Lesson 2의 Request 형식! \r\n, 그리고 끝의 빈 줄 \r\n\r\n 주의.
    string request =
        "GET /index.html HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "User-Agent: MyClient/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";
    send(sock, request.c_str(), (int)request.size(), 0);\
    cout << "------------보낸 요청------------" << request << "----------------------\n";

    // ---- 5) 응답(Response) 받기 ----
    char buffer[4096] = {0};
    int len = recv(sock, buffer, sizeof(buffer)-1, 0);
    if (len > 0){
        buffer[len] = '\0';
        cout << "----------- 받은 응답 -----------\n" << buffer << "\n----------------------\n";
    }

    // ---- 6) 정리 ----
    closesocket(sock);
    WSACleanup();
    return 0;
}