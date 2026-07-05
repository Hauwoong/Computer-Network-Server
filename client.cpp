// ==================================================================
//  client.cpp - 대화형 HTTP 클라이언트
//   - 서버(127.0.0.1:8080)에 한 번 연결(keep-alive)하고 메뉴를 반복한다.
//   - 사용자는 경로/폼 형식을 몰라도 되도록, 회원 번호·이름·나이만 입력하면
//     클라이언트가 GET/POST/PUT/DELETE 요청을 조립해 보낸다.
//   - 매 회 현재 회원 목록을 자동으로 보여주고, 응답을 화면에 출력한다.
// ==================================================================

#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

// send()는 요청한 바이트를 한 번에 다 못 보낼 수 있으므로, 전부 보낼 때까지 반복한다.
bool sendAll(SOCKET sock, const string& data)
{
    int totalSent = 0;
    while (totalSent < (int)data.size())
    {
        int sent = send(sock, data.c_str() + totalSent, (int)data.size() - totalSent, 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

// 서버로부터 "완전한 HTTP 응답 하나"를 정확히 읽어 돌려준다 (서버의 readOneRequest와 대칭).
// 헤더 끝(\r\n\r\n)까지 받고 Content-Length만큼 본문을 읽어 응답 하나의 경계를 잡는다.
// buf: 연결 동안 받은 바이트가 쌓이는 버퍼(다음 응답 조각이 미리 와 있을 수 있어 유지).
// 연결이 끊기면 빈 문자열("").
string readOneResponse(SOCKET sock, string& buf)
{
    // 1) 헤더 끝(\r\n\r\n)까지 받는다
    size_t headerEnd;
    while ((headerEnd = buf.find("\r\n\r\n")) == string::npos)
    {
        char tmp[4096];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return "";
        buf.append(tmp, n);
    }

    // 2) Content-Length 파악
    int contentLength = 0;
    size_t clPos = buf.find("Content-Length:");
    if (clPos == string::npos) clPos = buf.find("content-length:");
    if (clPos != string::npos && clPos < headerEnd)
        contentLength = atoi(buf.c_str() + clPos + 15);

    // 3) 헤더+본문 전체가 올 때까지 받는다
    size_t totalNeeded = headerEnd + 4 + contentLength;
    while (buf.size() < totalNeeded)
    {
        char tmp[4096];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return "";
        buf.append(tmp, n);
    }

    // 4) 응답 하나만 잘라 반환, 나머지는 buf에 남김
    string oneResp = buf.substr(0, totalNeeded);
    buf.erase(0, totalNeeded);
    return oneResp;
}

// HTTP 메시지에서 본문(빈 줄 뒤)만 잘라내는 도우미
string getBody(string msg)
{
    size_t pos = msg.find("\r\n\r\n");
    if (pos == string::npos) return "";
    return msg.substr(pos + 4);
}


int main() {
    SetConsoleOutputCP(CP_UTF8);   // 콘솔 출력 UTF-8
    SetConsoleCP(CP_UTF8);         // 콘솔 입력도 UTF-8 (한글 이름 입력이 안 깨지도록)

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

    // 이 연결 동안 받은 데이터가 쌓이는 버퍼 (readOneResponse가 사용)
    string buf = "";

    // 화면 출력용 구분선 (너비를 통일해 보기 좋게)
    string BAR(40, '=');
    string DASH(40, '-');

    // ---- 4) 메뉴 반복: 한 연결로 여러 요청을 보낸다 (keep-alive) ----
    while (true)
    {
        // (a) 현재 회원 목록을 먼저 보여준다 (서버 현황 파악)
        {
            string listReq =
                "GET /users HTTP/1.1\r\n"
                "Host: 127.0.0.1:8080\r\n"
                "\r\n";
            sendAll(sock, listReq);
            string listResp = readOneResponse(sock, buf);
            cout << "\n" << BAR << "\n";
            cout << " 현재 등록된 회원 목록\n";
            cout << BAR << "\n";
            string listBody = getBody(listResp);
            if (listBody.empty()) cout << " (비어 있음)\n";
            else                  cout << listBody;
            cout << BAR << "\n";
        }

        // (b) 메뉴
        cout << "\n어떤 요청을 보낼까요?\n";
        cout << " 1) 조회 (GET)\n";
        cout << " 2) 생성 (POST)\n";
        cout << " 3) 수정 (PUT)\n";
        cout << " 4) 삭제 (DELETE)\n";
        cout << " 9) 클라이언트만 종료 (서버는 계속 실행)\n";
        cout << " 0) 전체 종료 (서버까지 종료)\n";
        cout << " 번호 선택: ";
        int choice;
        cin >> choice;

        if (choice == 9) break;   // 클라이언트만 종료 (서버는 계속 실행)

        // 0번: 전체 종료 - 서버에 종료 요청을 보낸 뒤 클라이언트도 함께 종료
        if (choice == 0)
        {
            string request =
                "POST /shutdown HTTP/1.1\r\n"
                "Host: 127.0.0.1:8080\r\n"
                "\r\n";
            sendAll(sock, request);
            string response = readOneResponse(sock, buf);
            cout << "\n" << DASH << "\n";
            cout << " 받은 응답\n";
            cout << DASH << "\n";
            cout << response << "\n";
            cout << DASH << "\n";
            break;   // 서버가 종료됐으니 클라이언트도 종료
        }

        string method, path, body;
        if      (choice == 1) method = "GET";
        else if (choice == 2) method = "POST";
        else if (choice == 3) method = "PUT";
        else if (choice == 4) method = "DELETE";
        else { cout << "잘못된 번호\n"; continue; }

        // (c) 경로 결정 (사용자는 경로 형식을 몰라도 되게 한다)
        //  - POST(생성): 항상 /users 컬렉션 (id는 서버가 부여)
        //  - GET(조회): 회원 번호만 입력 (0이면 전체 목록)
        //  - PUT/DELETE: 대상 회원 번호만 입력
        if (method == "POST")
        {
            path = "/users";
            cout << "(POST는 /users 컬렉션에 생성합니다. id는 서버가 자동 부여)\n";
        }
        else if (method == "GET")
        {
            cout << "조회할 회원 번호를 입력하세요 (전체 목록은 0): ";
            int num;
            cin >> num;
            if (num == 0) path = "/users";
            else          path = "/users/" + to_string(num);
        }
        else   // PUT, DELETE
        {
            cout << "회원 번호를 입력하세요: ";
            int num;
            cin >> num;
            path = "/users/" + to_string(num);
            // (사전확인 제거) 입력한 번호를 그대로 서버로 보낸다.
            // 없는 회원이면 서버가 sqlite3_changes()==0 으로 404를, 잘못된 번호면 400을 응답한다.
        }

        // (d) POST/PUT은 이름·나이를 따로 입력받아 body를 자동 조립한다
        //     (사용자는 name=...&age=... 형식을 몰라도 됨)
        if (method == "POST" || method == "PUT")
        {
            string name, age;
            cout << "이름을 입력하세요: ";
            cin >> name;
            cout << "나이를 입력하세요: ";
            cin >> age;
            body = "name=" + name + "&age=" + age;   // 시스템이 폼 형식으로 변환
        }

        // (e) 요청 조립 — keep-alive이므로 Connection: close 는 넣지 않는다
        string request = method + " " + path + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:8080\r\n";
        if (!body.empty())
        {
            request += "Content-Type: application/x-www-form-urlencoded\r\n";
            request += "Content-Length: " + to_string(body.size()) + "\r\n";
        }
        request += "\r\n";
        request += body;

        // (f) 전송 + 응답 하나 받기
        sendAll(sock, request);
        cout << "\n" << DASH << "\n";
        cout << " 보낸 요청\n";
        cout << DASH << "\n";
        cout << request << "\n";
        cout << DASH << "\n";

        string response = readOneResponse(sock, buf);
        cout << "\n" << DASH << "\n";
        cout << " 받은 응답\n";
        cout << DASH << "\n";
        cout << response << "\n";
        cout << DASH << "\n";
    }

    // ---- 5) 종료: 반복이 끝나면 그때 연결을 닫는다 ----
    cout << "\n연결을 종료합니다.\n";

    // ---- 6) 정리 ----
    closesocket(sock);
    WSACleanup();
    return 0;
}