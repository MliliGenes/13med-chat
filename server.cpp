#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <ctime>
#include <cstring>
#include <csignal>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT        6767
#define BACKLOG     16
#define BUFSZ       4096
#define HISTORY_MAX 20

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
static const char* YELLOW  = "\033[33m";
static const char* CYAN    = "\033[36m";
static const char* WHITE   = "\033[97m";
static const char* GRAY    = "\033[90m";
static const char* MAGENTA = "\033[35m";
static const char* RST  = "\033[0m";
static const char* BOLD = "\033[1m";
static const char* DIM  = "\033[2m";

static const char* NICK_COLORS[] = {
    "\033[91m",
    "\033[92m",
    "\033[93m",
    "\033[94m",
    "\033[95m",
    "\033[96m",
};

static const int NUM_COLORS = 6;

// ── Data ─────────────────────────────────────────────────────────────────────
struct Client {
    int         fd;
    std::string nickname;
    std::string ip;
    std::string color;
};

std::vector<Client>     g_clients;
std::deque<std::string> g_history;

std::mutex g_mutex;

int g_color_idx = 0;

// ── Time ─────────────────────────────────────────────────────────────────────
static std::string ts() {
    time_t now = time(nullptr);

    tm* t = localtime(&now);

    char b[16];

    snprintf(
        b,
        sizeof(b),
        "%02d:%02d:%02d",
        t->tm_hour,
        t->tm_min,
        t->tm_sec
    );

    return b;
}

// ── Logging ──────────────────────────────────────────────────────────────────
static void log_info(const std::string& msg) {
    std::cout
        << GRAY << "[" << ts() << "] "
        << CYAN << "[INFO] "
        << RST << msg
        << std::endl;
}

static void log_ok(const std::string& msg) {
    std::cout
        << GRAY << "[" << ts() << "] "
        << GREEN << "[ OK ] "
        << RST << msg
        << std::endl;
}

static void log_warn(const std::string& msg) {
    std::cout
        << GRAY << "[" << ts() << "] "
        << YELLOW << "[WARN] "
        << RST << msg
        << std::endl;
}

static void log_error(const std::string& msg) {
    std::cout
        << GRAY << "[" << ts() << "] "
        << RED << "[FAIL] "
        << RST << msg
        << std::endl;
}

static void log_chat(
    const std::string& nick,
    const std::string& color,
    const std::string& msg
) {
    std::cout
        << GRAY << "[" << ts() << "] "
        << color << "<" << nick << ">"
        << RST << " "
        << WHITE << msg
        << RST
        << std::endl;
}

// ── Reliable socket send/recv ────────────────────────────────────────────────
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

// ── Broadcast ────────────────────────────────────────────────────────────────
static void broadcast(
    const std::string& msg,
    int skip_fd = -1,
    bool record = true
) {
    std::lock_guard<std::mutex> lk(g_mutex);

    for (auto& c : g_clients)
        if (c.fd != skip_fd)
            send_packet(c.fd, msg);

    if (record) {
        g_history.push_back(msg);

        if (g_history.size() > HISTORY_MAX)
            g_history.pop_front();
    }
}

// ── Client thread ────────────────────────────────────────────────────────────
void handle_client(Client client) {
    // nickname
    std::string nick;

    if (!recv_packet(client.fd, nick)) {
        close(client.fd);
        return;
    }

    while (!nick.empty() &&
          (nick.back() == '\n' ||
           nick.back() == '\r' ||
           nick.back() == ' '))
        nick.pop_back();

    client.nickname = nick.empty() ? "anon" : nick;

    {
        std::lock_guard<std::mutex> lk(g_mutex);

        client.color = NICK_COLORS[g_color_idx++ % NUM_COLORS];

        g_clients.push_back(client);
    }

    log_ok(
        client.color +
        client.nickname +
        RST +
        " joined from "
        + client.ip
    );

    // welcome
    {
        std::lock_guard<std::mutex> lk(g_mutex);

        std::string welcome =
            "\r\n"
            + std::string(BOLD)
            + "╔══════════════════════════════╗\r\n"
            + "║      SubnetChat v2.0        ║\r\n"
            + "╚══════════════════════════════╝\r\n"
            + RST
            + "\r\n"
            + " Welcome "
            + client.color
            + client.nickname
            + RST
            + "\r\n\r\n"
            + " /list              show users\r\n"
            + " /dm <nick> <msg>   private message\r\n\r\n";

        send_packet(client.fd, welcome);

        if (!g_history.empty()) {
            send_packet(
                client.fd,
                DIM
                + std::string("── Chat History ─────────────────────\r\n")
                + RST
            );

            for (auto& h : g_history)
                send_packet(client.fd, h);
        }
    }

    std::string join_msg =
        DIM
        + std::string("[")
        + ts()
        + "] *** "
        + client.nickname
        + " joined ***\r\n"
        + RST;

    broadcast(join_msg, client.fd);

    // chat loop
    while (true) {
        std::string text;

        if (!recv_packet(client.fd, text))
            break;

        while (!text.empty() &&
              (text.back() == '\n' ||
               text.back() == '\r'))
            text.pop_back();

        if (text.empty())
            continue;

        // /list
        if (text == "/list") {
            std::lock_guard<std::mutex> lk(g_mutex);

            std::string reply =
                BOLD
                + std::string("Online Users:\r\n")
                + RST;

            for (auto& c : g_clients) {
                reply += "  ";
                reply += c.color;
                reply += c.nickname;
                reply += RST;
                reply += "\r\n";
            }

            send_packet(client.fd, reply);

            continue;
        }

        // /dm
        if (text.rfind("/dm ", 0) == 0) {
            std::istringstream ss(text.substr(4));

            std::string target;
            std::string body;

            ss >> target;
            std::getline(ss, body);

            if (!body.empty() && body.front() == ' ')
                body.erase(body.begin());

            bool found = false;

            std::lock_guard<std::mutex> lk(g_mutex);

            for (auto& c : g_clients) {
                if (c.nickname == target) {
                    found = true;

                    std::string dm =
                        MAGENTA
                        + std::string("[DM] ")
                        + client.nickname
                        + " -> "
                        + target
                        + ": "
                        + body
                        + RST
                        + "\r\n";

                    send_packet(c.fd, dm);
                    send_packet(client.fd, dm);

                    break;
                }
            }

            if (!found) {
                send_packet(
                    client.fd,
                    RED
                    + std::string("[!] User not found\r\n")
                    + RST
                );
            }

            continue;
        }

        // message
        std::string msg =
            "["
            + ts()
            + "] "
            + client.color
            + client.nickname
            + RST
            + ": "
            + text
            + "\r\n";

        log_chat(client.nickname, client.color, text);

        broadcast(msg, client.fd);
    }

    // cleanup
    {
        std::lock_guard<std::mutex> lk(g_mutex);

        g_clients.erase(
            std::remove_if(
                g_clients.begin(),
                g_clients.end(),
                [&](const Client& c) {
                    return c.fd == client.fd;
                }),
            g_clients.end()
        );
    }

    close(client.fd);

    log_warn(client.nickname + " disconnected");

    broadcast(
        DIM
        + std::string("[")
        + ts()
        + "] *** "
        + client.nickname
        + " left ***\r\n"
        + RST
    );
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);

    if (srv < 0) {
        log_error("socket() failed");
        return 1;
    }

    int opt = 1;

    setsockopt(
        srv,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    sockaddr_in addr{};

    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(
            srv,
            reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)
        ) < 0) {

        log_error("bind() failed");
        return 1;
    }

    if (listen(srv, BACKLOG) < 0) {
        log_error("listen() failed");
        return 1;
    }

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

    std::cout
        << BOLD
        << WHITE
        << " Listening on "
        << GREEN
        << "0.0.0.0:"
        << PORT
        << RST
        << "\n\n";

    while (true) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);

        int cfd = accept(
            srv,
            reinterpret_cast<sockaddr*>(&peer),
            &plen
        );

        if (cfd < 0) {
            log_error("accept() failed");
            continue;
        }

        Client c;

        c.fd = cfd;
        c.ip = inet_ntoa(peer.sin_addr);

        log_info("Incoming connection from " + c.ip);

        std::thread(handle_client, c).detach();
    }

    close(srv);

    return 0;
}