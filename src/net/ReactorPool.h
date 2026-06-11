#pragma once

#include <thread>
#include <vector>

class EventLoop;

class Reactors {
public:
    explicit Reactors(int num);
    ~Reactors();

    int selectl(int connfd);
    std::vector<EventLoop*>& getreactors();

private:
    std::vector<EventLoop*> reactors;
    std::vector<std::thread> threads;
};
