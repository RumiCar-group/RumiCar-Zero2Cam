#include "telemetry_server.hpp"

#include <chrono>
#include <poll.h>
#include <stdexcept>

#include "../log.hpp"

namespace rumicar::net {

TelemetryServer::TelemetryServer(std::string bind_address, uint16_t port, size_t queue_frames)
    : bind_address_(std::move(bind_address)), port_(port), queue_frames_(queue_frames == 0 ? 1 : queue_frames) {}

TelemetryServer::~TelemetryServer() { stop(); }

void TelemetryServer::start() {
    listen_ = listen_tcp(bind_address_, port_, 4);
    port_ = bound_port(listen_.get());
    running_.store(true);
    acceptor_ = std::thread([this] { accept_loop(); });
    log::info("telemetry server listening on " + bind_address_ + ":" + std::to_string(port_));
}

void TelemetryServer::stop() {
    if (!running_.exchange(false)) return;
    listen_.shutdown();                          // wakes accept(); the fd is closed after the join
    if (acceptor_.joinable()) acceptor_.join();
    listen_.close();

    std::lock_guard<std::mutex> lock(clients_m_);
    for (auto& c : clients_) {
        {
            std::lock_guard<std::mutex> cl(c->m);
            c->closing = true;
        }
        c->fd.shutdown();
        c->cv.notify_all();
    }
    for (auto& c : clients_) {
        if (c->thread.joinable()) c->thread.join();
    }
    clients_.clear();
}

size_t TelemetryServer::client_count() const {
    std::lock_guard<std::mutex> lock(clients_m_);
    size_t n = 0;
    for (const auto& c : clients_) {
        std::lock_guard<std::mutex> cl(c->m);
        if (!c->closing) ++n;
    }
    return n;
}

void TelemetryServer::accept_loop() {
    while (running_.load()) {
        std::string peer;
        Fd fd = accept_client(listen_.get(), peer);
        if (!fd.valid()) break;
        set_timeouts(fd.get(), 2000, 0);
        set_nodelay(fd.get());

        auto c = std::make_unique<Client>();
        c->fd = std::move(fd);
        c->peer = peer;
        Client& ref = *c;
        std::lock_guard<std::mutex> lock(clients_m_);
        reap_locked();
        c->thread = std::thread([this, &ref] { client_loop(ref); });
        clients_.push_back(std::move(c));
        log::info("telemetry client connected: " + peer);
    }
}

void TelemetryServer::reap_locked() {
    for (auto it = clients_.begin(); it != clients_.end();) {
        bool finished;
        {
            std::lock_guard<std::mutex> cl((*it)->m);
            finished = (*it)->closing;
        }
        if (finished) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void TelemetryServer::client_loop(Client& c) {
    std::string why;
    while (true) {
        Frame frame;
        {
            std::unique_lock<std::mutex> lock(c.m);
            // Wake up periodically so a client that hung up is noticed even
            // while no frames are being published.
            c.cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return c.closing || !c.queue.empty(); });
            if (c.closing) break;
            if (c.queue.empty()) {
                lock.unlock();
                pollfd pfd{c.fd.get(), POLLRDHUP, 0};
                if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR)) != 0) {
                    why = "peer closed";
                    break;
                }
                continue;
            }
            frame = std::move(c.queue.front());
            c.queue.pop_front();
        }
        try {
            write_all(c.fd.get(), frame->data(), frame->size());
        } catch (const std::exception& e) {
            why = e.what();
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(c.m);
        c.closing = true;
        c.queue.clear();
    }
    c.fd.shutdown();
    log::info("telemetry client closed: " + c.peer + (why.empty() ? "" : " (" + why + ")"));
}

void TelemetryServer::publish(Frame frame) {
    std::lock_guard<std::mutex> lock(clients_m_);
    for (auto& c : clients_) {
        std::lock_guard<std::mutex> cl(c->m);
        if (c->closing) continue;
        if (c->queue.size() >= queue_frames_) {
            c->queue.pop_front();
            dropped_.fetch_add(1);
        }
        c->queue.push_back(frame);
        c->cv.notify_one();
    }
}

} // namespace rumicar::net
