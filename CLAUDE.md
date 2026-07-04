# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A university computer-networks assignment: a socket-based HTTP/1.1 server (`server.cpp`) and an interactive client (`client.cpp`) written in C++ for Windows (Winsock2), backed by SQLite. There is no test framework, no linter, and no build system — just direct `g++` invocations.

**Working style:** This is a learning assignment. The owner writes the code themselves and prefers step-by-step guidance ("change this here, and why") over having files rewritten wholesale. When helping, explain the change and let them type it; verify by building and running. Correctness/security bug fixes are the exception where editing directly is welcome.

## Build & run

SQLite is a C amalgamation and MUST be compiled with `gcc` (not `g++`) — under C++ its use of identifiers like `new` breaks. Compile it once to an object file, then link:

```sh
gcc -c sqlite3.c -o sqlite3.o                      # once (or when sqlite3.c changes)
g++ server.cpp sqlite3.o -o server.exe -lws2_32    # server (port 8080)
g++ client.cpp -o client.exe -lws2_32              # client
```

- Toolchain is MinGW-w64 UCRT g++ (posix threads → `std::thread` works). `-pthread` is optional.
- `#pragma comment(lib, "ws2_32.lib")` is MSVC-only; MinGW ignores it (harmless `-Wunknown-pragmas` warning) and requires `-lws2_32` on the command line.
- `data.db` is created at runtime; delete it to reset state. `*.exe`, `*.o`, and `data.db` are gitignored.

## Verifying changes (no test suite)

Run the server in the background and drive it with `curl`; each request opens its own connection but the server keep-alives, so curl reuses connections (`--next`, check `%{num_connects}`). The interactive client reads `cin`, so script it by piping newline-separated input (`printf '2\n/users\nname=kim\n0\n' | ./client.exe`). Give every `curl` a `--max-time`, and never use bare `wait` after backgrounding the server — `wait` also blocks on the never-exiting server; `wait` only on captured curl PIDs.

## Architecture

### server.cpp — multithreaded HTTP/1.1 server
Top-to-bottom: `sendAll()` → `readOneRequest()` → `buildResponse()` → `handleClient()` → `main()`.

- **`main`** does WSAStartup, opens `data.db` and creates the `users(id INTEGER PRIMARY KEY, data TEXT)` table, then `accept`s in a loop and hands each connection to `thread(handleClient, clientSock).detach()`.
- **`handleClient`** owns one connection: a keep-alive loop that reads one request at a time and responds, until the peer closes or sends `Connection: close`. It must `closesocket` at the end (omitting it leaks a handle per connection).
- **Concurrency model:** the global `sqlite3* db` is shared; a global `mutex dbMutex` is locked (`lock_guard`) **only around the `buildResponse` call**. This serializes DB work (and avoids a `sqlite3_last_insert_rowid` race) while keeping the slow `recv`/`send` outside the lock so slow/idle clients don't block others.
- **Message framing (`readOneRequest` / client's `readOneResponse`):** because connections persist, "read until EOF" is wrong. Read until `\r\n\r\n`, parse `Content-Length`, then read exactly that many body bytes; keep leftover bytes in a per-connection buffer for the next message. A `Content-Length` over 1 MB (or negative) is rejected with `413` before reading the body — this prevents a hang/DoS.

### buildResponse — routing (order matters)
Returns a complete HTTP response string. Two invariants that break silently if violated:
1. **Specific routes before catch-alls.** `GET /users` and `GET /users/{id}` must come before the file-serving `if (method == "GET")`, which serves any path and therefore must be the *last* GET handler. The `DELETE` case must come before the final 405/404 fallback, and DELETE must be in the 405 allow-condition, or it becomes unreachable.
2. **Every path returns.** The function ends with an unconditional `404`; the method-not-allowed check returns `405` with an `Allow` header.

Supported methods are **GET, POST, PUT, DELETE only** — HEAD was intentionally removed per the assignment.

### The graded requirement: errors reflect real state
Error responses must be derived from actual server state, never hardcoded/faked. Concretely: 404 comes from `sqlite3_changes()==0` (PUT/DELETE) or `sqlite3_step()==SQLITE_DONE` (GET one), 500 from checking `sqlite3_prepare_v2`/`sqlite3_step` return codes and echoing `sqlite3_errmsg(db)`, file 404 from `ifstream::is_open()`, and 400 from validating the `{id}` (digits only, ≤9 chars to avoid `stoi` overflow). Preserve this pattern in any new handler.

### Non-obvious gotchas
- **`ifstream` open check:** use `file.is_open()`, not `if (file)` — under this MinGW build `if(file)` is true even for a missing file (only `is_open()` is reliable).
- **File serving is whitelisted to `.html`** (else `403`); serving arbitrary paths previously leaked `server.cpp`, `data.db`, etc. `..` is also blocked (`400`).
- **`stoi` on `{id}`** must be guarded by a digits-only + length≤9 check first, or long/non-numeric ids crash the thread.
- All user data reaches SQL via `sqlite3_bind_*` prepared statements (no string concatenation) — keep it that way.
- `SetConsoleOutputCP(CP_UTF8)` and UTF-8 source are required for the Korean console/HTTP text.

### client.cpp — interactive keep-alive client
One persistent connection for the whole session. Each menu iteration first auto-sends `GET /users` to show current DB state, then lets the user pick a method (GET/POST/PUT/DELETE) and type an arbitrary path (+ body for POST/PUT). Mirrors the server's `sendAll` and Content-Length-framed reader. The design intent is that the client only chooses *what* to send and the server handles every path/error case.

## Design rationale

`README_문제해결_초안.md` is a detailed, numbered "problem → cause → fix" log (25+ entries) covering every non-trivial decision above. Consult and update it when changing behavior.
