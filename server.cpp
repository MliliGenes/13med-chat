#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <csignal>

// MSG_NOSIGNAL is Linux-only; on macOS use SO_NOSIGPIPE + flag 0
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define PORT      6767
#define BACKLOG   16
#define BUFSZ     2048

// ─────────────────────────────────────────────
struct Client {
    int         fd;
    std::string nickname;
    std::string ip;
};

std::vector<Client> g_clients;
std::mutex          g_mutex;

// ─────────────────────────────────────────────
// Current HH:MM timestamp
static std::string timestamp() {
    time_t now = time(nullptr);
    tm* t = localtime(&now);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    return buf;
}

// Send raw string to one fd
static void send_str(int fd, const std::string& s) {
    size_t total = 0;
    while (total < s.size()) {
        ssize_t n = send(fd, s.c_str() + total, s.size() - total, MSG_NOSIGNAL);
        if (n <= 0) break;
        total += n;
    }
}

// Broadcast to all clients (optionally skip one)
static void broadcast(const std::string& msg, int skip_fd = -1) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& c : g_clients)
        if (c.fd != skip_fd)
            send_str(c.fd, msg);
}

// ─────────────────────────────────────────────
void handle_client(Client client) {
    char buf[BUFSZ];

    // ── 1. Receive nickname (first packet) ──
    memset(buf, 0, BUFSZ);
    int n = recv(client.fd, buf, BUFSZ - 1, 0);
    if (n <= 0) { close(client.fd); return; }

    client.nickname = std::string(buf, n);
    // strip trailing whitespace / newlines
    while (!client.nickname.empty() &&
           (client.nickname.back() == '\n' || client.nickname.back() == '\r' ||
            client.nickname.back() == ' '))
        client.nickname.pop_back();

    if (client.nickname.empty()) client.nickname = "anon";

    // ── 2. Register & announce ──
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_clients.push_back(client);
        // tell new joiner how many people are here
        std::string welcome = "\r\n╔══ Welcome to SubnetChat ══╗\r\n"
                              "║  Nickname : " + client.nickname + "\r\n"
                              "║  Users online : " +
                              std::to_string(g_clients.size()) + "\r\n"
                              "╚═══════════════════════════╝\r\n\r\n";
        send_str(client.fd, welcome);
    }

    std::string join_msg = "  [" + timestamp() + "] *** " +
                           client.nickname + " joined ***\r\n";
    std::cout << join_msg;
    broadcast(join_msg, client.fd);   // everyone else sees it

    // ── 3. Chat loop ──
    while (true) {
        memset(buf, 0, BUFSZ);
        n = recv(client.fd, buf, BUFSZ - 1, 0);
        if (n <= 0) break;

        std::string text(buf, n);
        // trim trailing newline for clean formatting
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (text.empty()) continue;

        std::string msg = "  [" + timestamp() + "] " +
                          client.nickname + ": " + text + "\r\n";
        std::cout << msg;
        broadcast(msg, client.fd);
    }

    // ── 4. Cleanup on disconnect ──
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_clients.erase(
            std::remove_if(g_clients.begin(), g_clients.end(),
                [&](const Client& c) { return c.fd == client.fd; }),
            g_clients.end());
    }
    close(client.fd);

    std::string leave_msg = "  [" + timestamp() + "] *** " +
                            client.nickname + " left ***\r\n";
    std::cout << leave_msg;
    broadcast(leave_msg);
}

// ─────────────────────────────────────────────
int main() {
    signal(SIGPIPE, SIG_IGN); // don't crash on broken client pipes

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        { perror("bind"); return 1; }
    if (listen(srv, BACKLOG) < 0)
        { perror("listen"); return 1; }

    std::cout << "╔══════════════════════════════╗\n"
              << "║   SubnetChat server v1.0     ║\n"
              << "║   Listening on 0.0.0.0:" << PORT << "  ║\n"
              << "╚══════════════════════════════╝\n\n";

    while (true) {
        sockaddr_in peer{};
        socklen_t   plen = sizeof(peer);
        int cfd = accept(srv, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) { perror("accept"); continue; }

        Client c;
        c.fd = cfd;
        c.ip = inet_ntoa(peer.sin_addr);
        std::cout << "  [+] New connection from " << c.ip << "\n";

        std::thread(handle_client, c).detach();
    }

    close(srv);
}