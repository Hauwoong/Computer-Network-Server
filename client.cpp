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

    // ---- 4) 메뉴: 어떤 요청을 보낼지 고른다  ----
    cout << "\n어떤 요청을 보낼까요?\n";
    cout << " 1) GET /index.html  (200 OK)\n";
    cout << " 2) GET /users       (DB 조회 , 200)\n";
    cout << " 3) GET /nope        (404 Not Found)\n";
    cout << " 4) HEAD /index.html (헤더만)\n";
    cout << " 5) POST /users      (DB 저장, 201)\n";
    cout << " 6) PUT /users/1     (수정 , 200)\n";
    cout << " 7) POST /continue   (100 Continue)\n";
    cout << " 번호 선택: ";
    int choice;
    cin >> choice;

    // 선택에 따라 메서드/경로/본문을 정한다
    string method, path, body;
    if      (choice == 1) { method = "GET"; path = "/index.html"; body = ""; }
    else if (choice == 2) { method = "GET"; path = "/users"; body = ""; }
    else if (choice == 3) { method = "GET"; path = "/nope"; body = ""; }
    else if (choice == 4) { method = "HEAD"; path = "/index.html"; body = ""; }
    else if (choice == 5) { method = "POST"; path = "/users"; body = ""; }
    else if (choice == 6) { method = "PUT"; path = "/users/1"; body = ""; }
    else if (choice == 7) { method = "POST"; path = "/continue"; body = "hello"; }
    else { cout << "잘못된 번호\n"; closesocket(sock); WSACleanup(); return 1; }

    // POST 나 PUT은 원하는 데이터를 직접 입력
    if (method == "POST" || method == "PUT")
    {
        cout << "보낼 데이터를 입력하세요 (예: name=kim&age=20): ";
        cin >> body;
    }

    //요청 조립: 시작줄 + 헤더 + (본문 있으면 Content-Length) + 빈 줄 + 본문
    string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1:8080\r\n";
    if (!body.empty())  // POST/PUT 처럼 본문이 있을때만 길이를 알려준다
    {
        request += "Content-Type: application/x-www-form-urlencoded\r\n";
        request += "Content-Length: " + to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n";
    request += "\r\n";
    request += body;

    send(sock, request.c_str(), (int)request.size(), 0);
    cout << "------------보낸 요청------------\n" << request << "----------------------\n";

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