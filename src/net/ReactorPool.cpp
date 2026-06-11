#include "net/ReactorPool.h"

#include "net/EventLoop.h"

Reactors::Reactors(int num) {
    for (int i = 0; i < num; ++i) {
        reactors.emplace_back(new EventLoop);
        threads.emplace_back([i, this]() {
            this->reactors[i]->loop();
        });
    }
}

Reactors::~Reactors() {
    for (auto& reactor : reactors) {
        reactor->stop();
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    for (auto& reactor : reactors) {
        delete reactor;
    }
}

int Reactors::selectl(int connfd) {
    return connfd % 4;
}

std::vector<EventLoop*>& Reactors::getreactors() {
    return reactors;
}
