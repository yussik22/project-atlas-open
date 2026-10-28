#include <atomic>
#include <thread>

__declspec(noinline) void shared_work(std::atomic<int>& ready,
                                     std::atomic<bool>& go) {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    volatile int marker = 7;
    marker += 5;
}

int main() {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::thread first(shared_work, std::ref(ready), std::ref(go));
    std::thread second(shared_work, std::ref(ready), std::ref(go));
    while (ready.load() != 2) std::this_thread::yield();
    go.store(true);
    first.join();
    second.join();
    return 0;
}
