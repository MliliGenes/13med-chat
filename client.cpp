#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <csignal>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT  6767
#define BUFSZ 4096

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// ── XOR encryption ───────────────────────────────────────────────────────────
static const std::string XOR_KEY = "SubnetChat_k3y!";

static std::string xor_crypt(const std::string& data) {
    std::string out = data;

    for (size_t i = 0; i < out.size(); i++)
        out[i] ^= XOR_KEY[i % XOR_KEY.size()];

    return out;
}

// ── ANSI ─────────────────────────────────────────────────────────────────────
static const char* RED     = "\033[31m";
static const char* GREEN   = "\033[32m";
static const char* CYAN    = "\033[36m";
static const char* BLUE    = "\033[34m";
static const char* BOLD = "\033[1m";
static const char* RST  = "\033[0m";

// ── Globals ──────────────────────────────────────────────────────────────────
std::atomic<bool> g_running(true);
std::mutex g_console_mutex;

// ── Socket helpers ───────────────────────────────────────────────────────────
static bool send_all(int fd, const void* data, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t n = send(
            fd,
            (const char*)data + total,
            len - total,
            MSG_NOSIGNAL
        );

        if (n <= 0)
            return false;

        total += n;
    }

    return true;
}

static bool recv_all(int fd, void* data, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t n = recv(
            fd,
            (char*)data + total,
            len - total,
            0
        );

        if (n <= 0)
            return false;

        total += n;
    }

    return true;
}

// ── Packet send/recv ─────────────────────────────────────────────────────────
static bool send_packet(int fd, const std::string& msg) {
    std::string enc = xor_crypt(msg);

    uint32_t len = htonl((uint32_t)enc.size());

    if (!send_all(fd, &len, sizeof(len)))
        return false;

    if (!send_all(fd, enc.data(), enc.size()))
        return false;

    return true;
}

static bool recv_packet(int fd, std::string& out) {
    uint32_t len_net;

    if (!recv_all(fd, &len_net, sizeof(len_net)))
        return false;

    uint32_t len = ntohl(len_net);

    if (len > 1024 * 1024)
        return false;

    std::string enc(len, '\0');

    if (!recv_all(fd, enc.data(), len))
        return false;

    out = xor_crypt(enc);

    return true;
}

// ── UI ───────────────────────────────────────────────────────────────────────
static void draw_prompt() {
    std::cout
        << BOLD
        << BLUE
        << "❯ "
        << RST
        << std::flush;
}

static void banner() {
    std::cout
        << CYAN
        << R"(

   ███████╗██╗   ██╗██████╗ ███╗   ██╗███████╗████████╗
   ██╔════╝██║   ██║██╔══██╗████╗  ██║██╔════╝╚══██╔══╝
   ███████╗██║   ██║██████╔╝██╔██╗ ██║█████╗     ██║
   ╚════██║██║   ██║██╔══██╗██║╚██╗██║██╔══╝     ██║
   ███████║╚██████╔╝██████╔╝██║ ╚████║███████╗   ██║
   ╚══════╝ ╚═════╝ ╚═════╝ ╚═╝  ╚═══╝╚══════╝   ╚═╝

)"
        << RST;
}

// ── Receiver ─────────────────────────────────────────────────────────────────
void recv_loop(int sock) {
    while (g_running) {
        std::string msg;

        if (!recv_packet(sock, msg)) {
            std::lock_guard<std::mutex> lk(g_console_mutex);

            std::cout
                << "\r\033[K"
                << RED
                << "\n[!] Disconnected from server\n"
                << RST;

            g_running = false;

            break;
        }

        std::lock_guard<std::mutex> lk(g_console_mutex);

        std::cout << "\r\033[K";
        std::cout << msg;

        draw_prompt();
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    banner();

    if (argc < 2) {
        std::cerr
            << RED
            << "Usage: ./client <server_ip>\n"
            << RST;

        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);

    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP\n";
        return 1;
    }

    std::cout
        << GREEN
        << "[*] Connecting to "
        << argv[1]
        << ":"
        << PORT
        << "...\n"
        << RST;

    if (connect(
            sock,
            reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)
        ) < 0) {

        std::cerr
            << RED
            << "connect() failed\n"
            << RST;

        return 1;
    }

    std::string nick;

    std::cout
        << BOLD
        << "Enter nickname: "
        << RST;

    std::getline(std::cin, nick);

    if (nick.empty())
        nick = "anon";

    if (!send_packet(sock, nick)) {
        std::cerr << "failed sending nickname\n";
        return 1;
    }

    std::thread rx(recv_loop, sock);

    draw_prompt();

    std::string line;

    while (g_running) {
        if (!std::getline(std::cin, line))
            break;

        if (line.empty()) {
            draw_prompt();
            continue;
        }

        if (!send_packet(sock, line))
            break;

        draw_prompt();
    }

    g_running = false;

    shutdown(sock, SHUT_RDWR);

    close(sock);

    if (rx.joinable())
        rx.join();

    std::cout
        << GREEN
        << "\nbye.\n"
        << RST;

    return 0;
}