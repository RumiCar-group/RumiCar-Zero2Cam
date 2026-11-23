#include "control_server.hpp"

#include <chrono>
#include <cstring>
#include <sys/socket.h>

#include "../log.hpp"

namespace rumicar::net {

using json = nlohmann::json;

namespace {

std::string response_line(long long id, bool ok, const json& result, const std::string& error) {
    json r = {{"id", id}, {"ok", ok}};
    if (ok) {
        if (!result.is_null()) r["result"] = result;
    } else {
        r["error"] = error;
    }
    return r.dump() + "\n";
}

} // namespace

ControlServer::ControlServer(std::string bind_address, uint16_t port, Handler handler, ClientCallback on_client,
                             int client_timeout_ms)
    : bind_address_(std::move(bind_address)), port_(port), handler_(std::move(handler)), on_client_(std::move(on_client)),
      client_timeout_ms_(client_timeout_ms) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start() {
    listen_ = listen_tcp(bind_address_, port_, 2);
    port_ = bound_port(listen_.get());
    running_.store(true);
    acceptor_ = std::thread([this] { accept_loop(); });
    log::info("control server listening on " + bind_address_ + ":" + std::to_string(port_));
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    listen_.shutdown();                          // wakes accept(); the fd is closed after the join
    if (acceptor_.joinable()) acceptor_.join();
    listen_.close();
    {
        std::lock_guard<std::mutex> lock(client_fd_m_);
        if (client_fd_ >= 0) ::shutdown(client_fd_, SHUT_RDWR);
    }
    if (client_thread_.joinable()) client_thread_.join();
}

void ControlServer::push_event(const std::string& event, const json& data) {
    if (!client_active_.load()) return;
    const std::string line = json{{"event", event}, {"data", data}}.dump() + "\n";
    std::lock_guard<std::mutex> lock(events_m_);
    if (events_.size() >= kMaxQueuedEvents) events_.pop_front();
    events_.push_back(line);
}

void ControlServer::accept_loop() {
    while (running_.load()) {
        std::string peer;
        Fd fd = accept_client(listen_.get(), peer);
        if (!fd.valid()) break;
        set_timeouts(fd.get(), 2000, 100);
        set_nodelay(fd.get());
        set_user_timeout(fd.get(), client_timeout_ms_);

        if (client_active_.load()) {
            const std::string busy = response_line(0, false, nullptr, "busy: another client is connected");
            try { write_all(fd.get(), busy.data(), busy.size()); } catch (const std::exception&) {}
            // Let the refusal arrive before the connection ends: stop sending (the
            // line is followed by a FIN) and read what the client sends until it
            // closes, for at most 500 ms.  Closing with its hello still unread would
            // reset the connection, and the client (Windows) could lose the line.
            ::shutdown(fd.get(), SHUT_WR);
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            char sink[512];
            while (std::chrono::steady_clock::now() < until) {
                long n;
                try { n = read_some(fd.get(), sink, sizeof sink); } catch (const std::exception&) { break; }
                if (n == 0) break;   // the client has closed
            }
            log::warn("control client rejected (busy): " + peer);
            continue;
        }
        if (client_thread_.joinable()) client_thread_.join();   // previous client has finished
        {
            std::lock_guard<std::mutex> lock(events_m_);
            events_.clear();
        }
        client_active_.store(true);
        client_thread_ = std::thread([this, f = std::move(fd), peer]() mutable { client_loop(std::move(f), peer); });
    }
}

void ControlServer::client_loop(Fd fd, std::string peer) {
    {
        std::lock_guard<std::mutex> lock(client_fd_m_);
        client_fd_ = fd.get();
    }
    log::info("control client connected: " + peer);
    if (on_client_) on_client_(true, peer);

    std::string buffer;
    std::string why;
    bool hello_done = false;
    char chunk[4096];
    const auto idle_limit = std::chrono::milliseconds(client_timeout_ms_);
    auto last_rx = std::chrono::steady_clock::now();
    while (running_.load()) {
        long n;
        try {
            n = read_some(fd.get(), chunk, sizeof chunk);
        } catch (const std::exception& e) {
            why = e.what();
            break;
        }
        if (n == 0) break;   // peer closed
        if (n < 0 && std::chrono::steady_clock::now() - last_rx > idle_limit) {
            why = "nothing received for " + std::to_string(client_timeout_ms_) + " ms";
            break;
        }
        if (n > 0) {
            buffer.append(chunk, static_cast<size_t>(n));
            if (buffer.size() > kMaxLineBytes) {
                why = "line too long";
                break;
            }
            size_t pos;
            bool failed = false;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const std::string reply = handle_line(line, hello_done);
                try {
                    write_all(fd.get(), reply.data(), reply.size());
                } catch (const std::exception& e) {
                    why = e.what();
                    failed = true;
                    break;
                }
            }
            if (failed) break;
            // Counted from the end of the requests just handled: a request that
            // takes a while (activation) is not silence from the client.
            last_rx = std::chrono::steady_clock::now();
        }
        try {
            flush_events(fd.get());
        } catch (const std::exception& e) {
            why = e.what();
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(client_fd_m_);
        client_fd_ = -1;
    }
    fd.shutdown();
    fd.close();
    client_active_.store(false);
    {
        std::lock_guard<std::mutex> lock(events_m_);
        events_.clear();
    }
    log::info("control client closed: " + peer + (why.empty() ? "" : " (" + why + ")"));
    if (on_client_) on_client_(false, peer);
}

void ControlServer::flush_events(int fd) {
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(events_m_);
        pending.swap(events_);
    }
    for (const auto& line : pending) write_all(fd, line.data(), line.size());
}

std::string ControlServer::handle_line(const std::string& line, bool& hello_done) {
    json req;
    try {
        req = json::parse(line);
    } catch (const json::exception&) {
        return response_line(0, false, nullptr, "malformed JSON");
    }
    if (!req.is_object()) return response_line(0, false, nullptr, "request must be a JSON object");
    const auto id_it = req.find("id");
    if (id_it == req.end() || !id_it->is_number_integer()) return response_line(0, false, nullptr, "missing integer 'id'");
    const long long id = id_it->get<long long>();
    const auto type_it = req.find("type");
    if (type_it == req.end() || !type_it->is_string()) return response_line(id, false, nullptr, "missing string 'type'");
    const std::string type = type_it->get<std::string>();
    json params = json::object();
    if (const auto p = req.find("params"); p != req.end()) {
        if (!p->is_object()) return response_line(id, false, nullptr, "'params' must be an object");
        params = *p;
    }
    if (!hello_done && type != "hello") return response_line(id, false, nullptr, "hello first");

    try {
        const json result = handler_ ? handler_(type, params) : json();
        if (type == "hello") hello_done = true;
        return response_line(id, true, result, "");
    } catch (const RequestError& e) {
        return response_line(id, false, nullptr, e.what());
    } catch (const std::exception& e) {
        log::error("request '" + type + "' failed: " + e.what());
        return response_line(id, false, nullptr, std::string("internal error: ") + e.what());
    }
}

} // namespace rumicar::net
