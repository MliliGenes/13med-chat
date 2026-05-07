#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <csignal>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define PORT  6767
#define BUFSZ 2048

std::atomic<bool> g_running(true);

// ─────────────────────────────────────────────
// Receiver thread: prints incoming messages above the prompt
void recv_loop(int sock) {
    char buf[BUFSZ];
    while (g_running) {
        memset(buf, 0, BUFSZ);
        int n = recv(sock, buf, BUFSZ - 1, 0);
        if (n <= 0) {
            std::cout << "\r\n[!] Disconnected from server.\r\n";
            g_running = false;
            break;
        }
        // Move up, clear line, print message, redraw prompt
        std::cout << "\r\033[K"          // clear current input line
                  << std::string(buf, n) // the message (already has \r\n)
                  << "> "                // redraw prompt
                  << std::flush;
    }
}

// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./client <server_ip>\n"
                  << "  e.g. ./client 192.168.1.42\n";
        return 1;
    }

    // ── Connect ──
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) <= 0) {
        std::cerr << "[!] Bad IP address: " << argv[1] << "\n";
        return 1;
    }

    std::cout << "Connecting to " << argv[1] << ":" << PORT << " ...\n";
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[!] connect");
        return 1;
    }
    std::cout << "Connected!\n\n";

    // ── Pick nickname ──
    std::string nickname;
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    if (nickname.empty()) nickname = "anon";
    send(sock, nickname.c_str(), nickname.size(), MSG_NOSIGNAL);

    // ── Start receiver thread ──
    std::thread rx(recv_loop, sock);

    // ── Send loop ──
    std::string line;
    while (g_running) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (!g_running) break;
        if (line.empty()) continue;

        line += '\n';
        ssize_t sent = send(sock, line.c_str(), line.size(), MSG_NOSIGNAL);
        if (sent <= 0) break;
    }

    g_running = false;
    shutdown(sock, SHUT_RDWR);
    close(sock);
    rx.join();

    std::cout << "\nBye!\n";
    return 0;
}