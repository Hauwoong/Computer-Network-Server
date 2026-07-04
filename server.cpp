//==========================================================
// server.cpp - 소켓 기반 HTTP/1.1 서버
//  - 포트 8080에서 TCP 연결을 받아 HTTP 요청을 직접 파싱한다.
//  - 회원(users) 자원에 대한 GET/POST/PUT/DELETE를 SQLite로 처리한다.
//  - 오류는 하드코딩이 아니라 실제 서버 상태(DB 처리 결과·파일 존재 여부 등)로
//    판단해 알맞은 상태 코드(200/201/400/403/404/405/413/500)를 회신한다.
//  - 연결마다 스레드를 만들어 동시 접속을 처리하고(멀티스레드),
//    한 연결에서 여러 요청을 주고받는다(HTTP/1.1 keep-alive).
//==========================================================

#include <iostream> // 화면 출력 cout
#include <string> // std::string 문자열
#include <sstream> // istringstream 쓰기 위한 헤더
#include "sqlite3.h" // SQLite 데이터베이스
#include <winsock2.h> // 윈도우 소켓 함수들 (socket, bind, ...)
#include <ws2tcpip.h> // 주소 관련 보조 함수
#include <windows.h> // SetConsoleOutputCP 사용 (winsock2.h 다음에 include!)
#include <fstream>  // 파일 읽기 ifstream
#include <thread>   // std::thread
#include <mutex>   // std::mutex

#pragma comment(lib, "ws2_32.lib") // ws2_32 라이브러리를 같이 링크 (MSVC용 표시)

using namespace std;

// ---- 전역 상태 (모든 연결 스레드가 공유) ----
sqlite3* db;      // SQLite 연결 핸들 (main에서 한 번 열고 이후 계속 사용)
mutex dbMutex;    // 여러 스레드가 동시에 DB를 만지지 않도록 보호 (데이터 경합 방지)
mutex coutMutex;  // 여러 스레드의 로그 출력이 한 줄씩 완결되도록 보호 (줄 섞임 방지)

// send()는 요청한 바이트를 한 번에 다 못 보낼 수 있으므로(소켓 버퍼가 차면 일부만 전송),
// 전부 보낼 때까지 반복한다. 응답이 커도 잘리지 않도록 보장. 실패 시 false.
bool sendAll(SOCKET sock, const string& data)
{
    int totalSent = 0;  // 총 전송한 바이트 수
    while (totalSent < (int)data.size()) 
    {
        int sent = send(sock, data.c_str() + totalSent, (int)data.size() - totalSent, 0); // 남은 데이터 전송
        if (sent <= 0) return false; // 전송 실패
        totalSent += sent;
    }
    return true; // 전송 성공
}

// 한 연결(sock)에서 "완전한 HTTP 요청 하나"를 정확히 읽어 돌려준다.
// keep-alive에서는 연결이 안 끊기므로 "끝까지 읽기"가 불가능하다. 그래서 헤더 끝(\r\n\r\n)을
// 찾고 Content-Length만큼 본문을 읽어 요청 하나의 경계를 잡아낸다(=메시지 프레이밍).
// buf: 이 연결에서 받은 바이트가 쌓이는 버퍼. 다음 요청 조각이 미리 와 있을 수 있어 유지한다.
// 반환: 요청 문자열 하나. 연결이 끊기면 빈 문자열("").
string readOneRequest(SOCKET sock, string& buf)
{
    // 1) 헤더의 끝(\r\n\r\n)이 buf에 들어올 때까지 계속 받는다
    size_t headerEnd;
    while ((headerEnd = buf.find("\r\n\r\n")) == string::npos)
    {
        char tmp[4096];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return "";   // 상대가 연결을 닫음
        buf.append(tmp, n);
    }

    // 2) 헤더에서 Content-Length 값을 찾는다 (없으면 본문 0)
    int contentLength = 0;
    size_t clPos = buf.find("Content-Length:");
    if (clPos == string::npos) clPos = buf.find("content-length:"); // 소문자도 대비
    if (clPos != string::npos && clPos < headerEnd)
    {
        // "Content-Length:" 는 15글자 -> 그 뒤 숫자를 읽는다 (atoi가 앞 공백은 무시)
        contentLength = atoi(buf.c_str() + clPos + 15);
    }

    // 본문이 상한을 넘거나(악의적 큰 값) 음수면 -> 413 보내고 이 연결 중단.
    // 거대한 본문을 받으려 무한 대기(hang)하는 것을 원천 차단한다.
    const int MAX_BODY = 1024 * 1024;   // 1MB
    if (contentLength < 0 || contentLength > MAX_BODY)
    {
        string msg = "요청 본문이 너무 큽니다. (최대 1MB)";
        string resp =
            "HTTP/1.1 413 Payload Too Large\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + msg;
        sendAll(sock, resp);
        return "";   // 빈 문자열 반환 -> main 루프가 이 연결을 닫는다
    }

    // 3) 요청 전체 크기 = 헤더끝(+4) + 본문 길이. 다 받을 때까지 더 받는다
    size_t totalNeeded = headerEnd + 4 + contentLength;
    while (buf.size() < totalNeeded)
    {
        char tmp[4096];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return "";
        buf.append(tmp, n);
    }

    // 4) 요청 하나만 잘라내고, 나머지(다음 요청 조각)는 buf에 남긴다
    string oneReq = buf.substr(0, totalNeeded);
    buf.erase(0, totalNeeded);
    return oneReq;
}

// 저장된 폼 데이터("name=박준웅&age=10")를 보기 좋게 변환한다.
// '=' -> ' : ',  '&' -> '  '   =>   "name : 박준웅  age : 10"
string formatData(const string& data)
{
    string result;
    for (char c : data)
    {
        if (c == '=')      result += " : ";
        else if (c == '&') result += "  ";
        else               result += c;
    }
    return result;
}

// 메서드와 경로를 보고 알맞은 HTTP 응답 문자열 전체를 만들어 돌려주는 라우팅 함수.
// 검사 순서가 중요하다: 구체적인 경로(/users, /users/{id})를 먼저 보고,
// 아무 경로나 잡는 파일 서빙(GET)과 405/404 fallback은 반드시 뒤에 둔다.
// 그래야 /users 요청이 파일 서빙에 먼저 가로채이지 않는다.
string buildResponse(string method, string path, string body)
{
    // [케이스 1] GET /users -> DB에서 회원 목록 꺼내기 (SELECT)
    if (method == "GET" && path == "/users")
    {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT id, data FROM users;", -1, &stmt, nullptr) != SQLITE_OK)
        {
            string msg = "서버 DB 오류(prepare 실패): ";
            msg += sqlite3_errmsg(db);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        string list = "";
        // step이 SQLITE_ROW를 주는 동안 = 읽을 행이 남아있는 동안 반복
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
           int id = sqlite3_column_int(stmt, 0);
           const unsigned char* data = sqlite3_column_text(stmt, 1);
           string dataStr = data ? (const char*)data : "";
           list += to_string(id) + "번: " + formatData(dataStr) + "\n";
        }
        sqlite3_finalize(stmt);

        return 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(list.size()) + "\r\n"
            "\r\n" + list;
    }

    if (method == "GET" && path.rfind("/users/", 0) == 0) // "/users/" 로 시작하면 뒤에 붙은 번호를 id로 사용
    {
        string idStr = path.substr(7); // "/users/" 는 7글자 -> 그 뒤가 id

        if (idStr.empty() || idStr.find_first_not_of("0123456789") != string::npos || idStr.size() > 9) // 숫자가 아닌 글자가 있으면
        {
            string msg = "잘못된 회원 번호입니다: " + idStr;
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        int id = stoi(idStr);   // 여기서는 이미 숫자없이 들어온 경우는 없으므로 stoi로 변환 가능

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT data FROM users WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK)
        {
            string msg = "서버 DB 오류(prepare 실패): ";
            msg += sqlite3_errmsg(db);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        sqlite3_bind_int(stmt, 1, id); // ? 자리에 id

        if (sqlite3_step(stmt) == SQLITE_ROW)   // step이 SQLITE_ROW를 주면 = 읽을 행이 있으면
        {
            const unsigned char* data = sqlite3_column_text(stmt, 0);   //0번 칸: data
            string result = formatData(data ? (const char*)data : "");  // NULL이면 빈 문자열, 보기 좋게 변환
            sqlite3_finalize(stmt);
            return 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(result.size()) + "\r\n"
                "\r\n" + result;
        }
        else    // step이 SQLITE_DONE을 주면 = 읽을 행이 없으면 = id번 회원이 없으면
        {
            sqlite3_finalize(stmt);
            string msg = to_string(id) + "번 회원이 없습니다.";
            return
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }
    }

    // [케이스 2] 그 밖의 GET -> 요청 경로를 실제 파일로 열어본다 (GET 중 맨 마지막)
    if (method == "GET")
    {
        // 경로 앞의 "/"를 떼고 파일 이름으로 사용 ("/index.html" -> "index.html")
        string filename = path.substr(1);

        // 루트 경로("/")로 오면 홈페이지(index.html)로 서빙한다.
        if (filename.empty())
        {
            filename = "index.html";
        }

        // 보안: 상위 폴더로 빠져나가는 경로(../)는 거부 -> 400
        if (filename.find("..") != string::npos)
        {
            string msg = "잘못된 경로입니다.";
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        // 보안: .html 파일만 서빙 허용 (소스코드/DB 등 다른 파일 노출 방지)
        bool isHtml = filename.size() >= 5 && filename.substr(filename.size() - 5) == ".html";
        if (!isHtml)
        {
            string msg = "허용되지 않는 파일 형식입니다. (.html만 접근 가능)";
            return
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        // 파일을 바이너리로 연다 (실제 서버 상태 = 파일 존재 여부 확인)
        ifstream file(filename, ios::binary);
        if (file.is_open()) // 파일이 실제로 열렸다 = 존재한다 -> 200 OK
        {
            stringstream ss;
            ss << file.rdbuf(); // 파일 내용을 모두 읽어 문자열로 만든다
            string content = ss.str();
            return
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + to_string(content.size()) + "\r\n"
                "\r\n" + content;
        }
        else    // 파일이 실제로 없으면 -> 실제 상태 기반 404 Not Found
        {
            string html = "<h1>404 - 파일을 찾을 수 없습니다.</h1>";
            return
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + to_string(html.size()) + "\r\n"
                "\r\n" + html;
        }
    }

    // [케이스 3] POST /users -> DB에 저장 후 201 Created (Location으로 위치 안내)
    if (method == "POST" && path == "/users")
    {
        // (1) 본문이 비어 있으면 생성할 데이터가 없음 -> 400 Bad Request
        if (body.empty())
        {
            string msg = "생성할 데이터(본문)가 없습니다.";
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "INSERT INTO users (data) VALUES (?);", -1, &stmt, nullptr) != SQLITE_OK)
        {
            string msg = "서버 DB 오류(prepare 실패): ";
            msg += sqlite3_errmsg(db);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        sqlite3_bind_text(stmt, 1, body.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE)
        {
            string msg = "서버 DB 오류(step 실패): ";
            msg += sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        sqlite3_finalize(stmt);

        // (2) 방금 INSERT된 행의 id를 알아내 Location 헤더로 알려준다
        int newId = (int)sqlite3_last_insert_rowid(db);

        string location = "/users/" + to_string(newId);
        string msg = to_string(newId) + "번 회원 생성됨! 데이터: " + body;
        return
            "HTTP/1.1 201 Created\r\n"
            "Location: " + location + "\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "\r\n" + msg;
    }

    // [케이스 4] PUT /users/{id} -> 있으면 200, 없으면 404, DB오류면 500
    // 경로가 "/users/" 로 시작하면 뒤에 붙은 번호를 id로 사용
    if (method == "PUT" && path.rfind("/users/", 0) == 0)
    {
        // ID 추출
        string idStr = path.substr(7); // "/users/" 는 7글자 -> 그 뒤가 id

        // id가 숫자가 아니면(빈 값 포함) 또는 너무 길면 잘못된 요청 -> 400 Bad Request
        if (idStr.empty() || idStr.find_first_not_of("0123456789") != string::npos || idStr.size() > 9)
        {
            string msg = "잘못된 회원 번호입니다: " + idStr;
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        // 수정할 데이터(본문)가 없으면 -> 400 Bad Request (POST와 동일 정책)
        if (body.empty())
        {
            string msg = "수정할 데이터(본문)가 없습니다.";
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        int id = stoi(idStr); // 여기서는 이미 숫자임이 보장됨 -> 예외 안 남

        sqlite3_stmt* stmt;
        // prepare: INSERT 문장 준비 (?는  나중에 채울 자리)
        if (sqlite3_prepare_v2(db, "UPDATE users SET data = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK)
        {
            string msg = "서버 DB 오류(prepare 실패): ";
            msg += sqlite3_errmsg(db);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        // bind: 첫 번째 ? 자리에 body를 안전하게 꽂는다
        sqlite3_bind_text(stmt, 1, body.c_str(), -1, SQLITE_TRANSIENT); // ? 자리에 새 데이터
        sqlite3_bind_int(stmt, 2, id); // ? 자리에 id

        // step 실패 -> UPDATE 실행 자체가 실패 -> 500
        if (sqlite3_step(stmt) != SQLITE_DONE)
        {
            string msg = "서버 DB 오류(step 실패): ";
            msg += sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        // finalize: 정리
        sqlite3_finalize(stmt);

        // 실제로 몇 행이 바뀌었는지 데이터 베이스 서버 상태를 확인
        int changed = sqlite3_changes(db);
        
        if (changed == 0) //  id번 회원이 실제로 없어서 아무것도 안 바뀜
        {
            string msg = to_string(id) + "번 회원이 없어서 수정할 수 없습니다.";
            return
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }
        
        // 실제로 id번 회원이 있어서 수정됨
        string msg = to_string(id) + "번 회원 수정됨! 새 데이터: " + body;
        return
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "\r\n" + msg;
    }

    // [케이스 5] DELETE /users/{id} -> 실제로 지워지면 200, 없으면 404, DB오류면 500
    if (method == "DELETE" && path.rfind("/users/", 0) == 0)
    {
        string idStr = path.substr(7); // "/users/" 다음이 id

        // id가 숫자가 아니거나(빈 값 포함) 너무 길면 -> 400 Bad Request
        if (idStr.empty() || idStr.find_first_not_of("0123456789") != string::npos || idStr.size() > 9)
        {
            string msg = "잘못된 회원 번호입니다: " + idStr;
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        int id = stoi(idStr);

        sqlite3_stmt* stmt;
        // prepare 실패 -> 500
        if (sqlite3_prepare_v2(db, "DELETE FROM users WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK)
        {
            string msg = "서버 DB 오류(prepare 실패): ";
            msg += sqlite3_errmsg(db);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        sqlite3_bind_int(stmt, 1, id);

        // step 실패 -> DELETE 실행 자체가 실패 -> 500
        if (sqlite3_step(stmt) != SQLITE_DONE)
        {
            string msg = "서버 DB 오류(step 실패): ";
            msg += sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        sqlite3_finalize(stmt);

        // 실제로 몇 행이 지워졌는지 확인 (실제 서버 상태 기반)
        int changed = sqlite3_changes(db);

        if (changed == 0)   // 그 번호 회원이 실제로 없어서 아무것도 안 지워짐
        {
            string msg = to_string(id) + "번 회원이 없어서 삭제할 수 없습니다.";
            return
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
        }

        string msg = to_string(id) + "번 회원 삭제됨!";
        return
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "\r\n" + msg;
    }

    // 지원하지 않는 메서드 -> 405 Method Not Allowed
    if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE")
    {
        string msg = "지원하지 않는 메서드입니다: " + method;
        return
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET, POST, PUT, DELETE\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + to_string(msg.size()) + "\r\n"
            "\r\n" + msg;
    }

    // 메서드는 지원하지만 경로가 맞지 않음 -> 404 Not Found (항상 return하여 함수 종료)
    string notFoundMsg = "요청한 경로를 찾을 수 없습니다: " + path;
    return
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: " + to_string(notFoundMsg.size()) + "\r\n"
        "\r\n" + notFoundMsg;
}

// 한 연결을 전담 처리하는 함수 (연결마다 별도 스레드에서 실행된다)
void handleClient(SOCKET clientSock)
{
    { lock_guard<mutex> lk(coutMutex); cout << "\n[새 연결] 클라이언트 접속됨\n"; }

    // 이 연결 동안 받은 바이트가 쌓이는 버퍼. readOneRequest가 이 버퍼를 공유하며
    // 한 요청씩 잘라 쓰고, 남은 조각(다음 요청)은 여기에 남겨 둔다.
    string buf = "";

    // 한 연결에서 요청이 오는 동안 계속 처리 (keep-alive)
    while (true) {
        string requestStr = readOneRequest(clientSock, buf);
        if (requestStr.empty()) break;   // 상대가 연결을 닫음 -> 이 연결 처리 종료

        // 요청 첫 줄 "METHOD PATH VERSION"을 공백 기준으로 세 조각으로 분리
        istringstream iss(requestStr);
        string method, path, version;
        iss >> method >> path >> version;
        { lock_guard<mutex> lk(coutMutex); cout << "[요청] 메서드=" << method << "  경로=" << path << "  버전=" << version << "\n"; }

        // 헤더와 본문의 경계(\r\n\r\n) 다음부터가 본문(POST/PUT의 데이터)
        string body = "";
        size_t pos = requestStr.find("\r\n\r\n");
        if (pos != string::npos)
            body = requestStr.substr(pos + 4);

        // 서버 종료 요청 (제어용 엔드포인트, 데모용) -> 응답 후 프로세스 전체 종료
        if (method == "POST" && path == "/shutdown")
        {
            string msg = "서버를 종료합니다.";
            string resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: " + to_string(msg.size()) + "\r\n"
                "\r\n" + msg;
            sendAll(clientSock, resp);       // 종료 안내를 먼저 보내고
            closesocket(clientSock);
            { lock_guard<mutex> lk(coutMutex); cout << "[종료] 클라이언트 요청으로 서버를 종료합니다.\n"; }
            exit(0);                          // 프로세스 전체 종료 (모든 스레드 함께 종료)
        }

        // DB를 만지는 buildResponse 구간만 잠근다 (한 번에 한 스레드만)
        string response;
        {
            lock_guard<mutex> lock(dbMutex);
            response = buildResponse(method, path, body);
        }
        sendAll(clientSock, response);

        if (requestStr.find("Connection: close") != string::npos ||
            requestStr.find("connection: close") != string::npos)
            break;
    }

    closesocket(clientSock);   // 이 연결만 닫음 (소켓 누수 방지)
    { lock_guard<mutex> lk(coutMutex); cout << "[연결 종료]\n"; }
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

    // ---- 5) 반복: 연결을 계속 받는다 (한 연결에서 여러 요청 처리) ----
    while (1) {
        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) continue;

        // 스레드 생성이 실패해도(자원 고갈 등) 서버가 죽지 않도록 예외를 처리한다
        try {
            thread(handleClient, clientSock).detach();   // 이 연결은 새 스레드가 전담, 메인은 즉시 다음 accept로
        } catch (const exception& e) {
            { lock_guard<mutex> lk(coutMutex); cout << "[경고] 스레드 생성 실패, 연결 거절: " << e.what() << "\n"; }
            closesocket(clientSock);   // 처리 못 하는 연결은 닫아 소켓 누수를 막는다
        }
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}