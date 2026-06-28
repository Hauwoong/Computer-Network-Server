//==========================================================
// server.cpp - 최소 동작 서버
// 연결을 1번 받아서, 받은 요청을 화면에 출력하고,
// 간단한 200 OK 응답을 보낸 뒤 종료한다.
//==========================================================

#include <iostream> // 화면 출력 cout
#include <string> // std::string 문자열
#include <sstream> // istringstream 쓰기 위한 헤더
#include "sqlite3.h" // SQLite 데이터베이스
#include <winsock2.h> // 윈도우 소켓 함수들 (socket, bind, ...)
#include <ws2tcpip.h> // 주소 관련 보조 함수
#include <windows.h> // SetConsoleOutputCP 사용 (winsock2.h 다음에 include!)

#pragma comment(lib, "ws2_32.lib") // ws2_32 라이브러리를 같이 링크 (MSVC용 표시)

using namespace std;

sqlite3* db; // 데이터베이스 포인터

// 메서드와 경로를 보고, 알맞은 응답 문자열을 만들어 돌려주는 함수
string buildResponse(string method, string path, string body)
{
    // [케이스 1] GET /index.html -> 200 OK (본문 있음)
    if (method == "GET" && path == "/index.html")
    {
        string html = "<h1>Hello! 메인 페이지입니다.</h1>";
        return
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + to_string(html.size()) + "\r\n"
            "\r\n" + html;
    }

    // [케이스 2] GET /users -> DB에서 회원 목록 꺼내기 (SELECT)
    if (method == "GET" && path == "/users")
    {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "SELECT id, data FROM users;", -1, &stmt, nullptr);

        string list = "";
        // step이 SQLITE_ROW를 주는 동안 = 읽을 행이 남아있는 동안 반복
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
            int id = sqlite3_column_int(stmt, 0);
            const unsigned char* data = sqlite3_column_text(stmt, 1);   //0번 칸: id
            list += to_string(id) + "번: " + (const char*)data + "\n";  //1번 칸: data
        }
        sqlite3_finalize(stmt);

        return 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(list.size()) + "\r\n"
            "\r\n" + list;
    }

    // [케이스 3] 그 외 모든 GET -> 404 Not Found
    if (method == "GET")
    {
        string html = "<h1>404 - 페이지를 찾을 수 없습니다.</h1>";
        return
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + to_string(html.size()) + "\r\n"
            "\r\n" + html;
    }

    // [케이스 4] HEAD /index.html -> 200 OK, 헤더만 본문 없음
    if (method == "HEAD" && path == "/index.html")
    {
        string html = "<h1>Hello! 메인 페이지입니다.</h1>"; // 본문 크기 계산용
        return 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + to_string(html.size()) + "\r\n"
            "\r\n";     // 본문이 필요 없으니 html은 붙이지 않는다
    }

    // [케이스 5] POST /users -> DB에 저장 후 201 Created (새 자원 생성됨)
    if (method == "POST" && path == "/users")
    {
        // prepare: INSERT 문장 준비 (?는  나중에 채울 자리)
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "INSERT INTO users (data) VALUES (?);", -1, &stmt, nullptr);

        // bind: 첫 번째 ? 자리에 body를 안전하게 꽂는다
        sqlite3_bind_text(stmt, 1, body.c_str(), -1, SQLITE_TRANSIENT);

        // step: 실행 (실제로 INSERT 됨)
        sqlite3_step(stmt);

        // finalize: 정리
        sqlite3_finalize(stmt);
        
        // 받은 본문(body)을 그래로 확인용으로 돌려준다
        string msg = "DB에 저장됨! 데이터: " + body;
        return 
            "HTTP/1.1 201 Created\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "\r\n" + msg;
    }

    // [케이스 6] PUT /users/1 -> 200 OK (기존 자원 수정됨)
    if (method == "PUT" && path == "/users/1")
    {
        sqlite3_stmt* stmt;
        // prepare: INSERT 문장 준비 (?는  나중에 채울 자리)
        sqlite3_prepare_v2(db, "UPDATE users SET data = ? WHERE id = 1;", -1, &stmt, nullptr);

         // bind: 첫 번째 ? 자리에 body를 안전하게 꽂는다
        sqlite3_bind_text(stmt, 1, body.c_str(), -1, SQLITE_TRANSIENT); // ? 자리에 새 데이터

        // step: 실행 (실제로 INSERT 됨)
        sqlite3_step(stmt);

        // finalize: 정리
        sqlite3_finalize(stmt);

        string msg = "1번 회원 수정됨! 새 데이터: " + body;
        return
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "\r\n" + msg;
    }

    // 아직 처리 못한 메서드 임시 응답 (다음 단계에서 채움 )
    return "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
}

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

    // ----DB 열기 + users 표 만들기 ----
    if (sqlite3_open("data.db", &db) != SQLITE_OK)
    {
        cout << "DB 열기 실패\n";
        return 1;
    }
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, data TEXT);";
    sqlite3_exec(db, createSql, nullptr, nullptr, nullptr); // SQL 한 줄 실행
    cout << "DB 준비 완료 (data.db)\n";

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

        cout << "\n[요청 도착] 메서드=" << method << "  경로=" << path << "  버전=" << version << "\n";
    
        // --- 8) 응답: buildResponse 함수에게 맡긴다 ---
        string body = "";
        size_t pos = requestStr.find("\r\n\r\n"); //헤더와 본문의 경계를 찾는다
        if (pos != string::npos)    //경계를 찾았으면
        {
            body = requestStr.substr(pos + 4);  // 그 4글자 뒤부터 끝까지 = 본문
        }

        // [케이스 6] 100 Continue 데모: 중간 응답 먼저 -> 최종 응답 나중
        if (method == "POST" && path == "/continue")
        {
            // (1) 중간 응답: "계속 보내도 돼" 신호 (본문 없이 빈 줄로 끝)
            string interim = "HTTP/1.1 100 Continue\r\n\r\n";
            send(clientSock, interim.c_str(), (int)interim.size(), 0);

            // (2) 최종 응답: 진짜 결과
            string finalBody = "100 Continue 받은 뒤의 최종 응답입니다!";
            string finalResp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(finalBody.size()) + "\r\n"
                "\r\n" + finalBody;
            send(clientSock, finalResp.c_str(), (int)finalResp.size(), 0);

            closesocket(clientSock);    // 이 연결 닫고
            continue;   // 아래 일반 응답 로직은 건너뛴다!
        }

        string response = buildResponse(method, path, body);
        send(clientSock, response.c_str(), (int)response.size(), 0);

        // --- 9) 이 연결만 닫는다 (서버는 계속 살아있음) ---
        closesocket(clientSock);
    }


    
    closesocket(listenSock);
    WSACleanup();
    return 0;
}