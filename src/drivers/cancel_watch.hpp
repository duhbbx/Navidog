#pragma once
#include "navidog/types.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
namespace navidog::native {
// Only a native driver's documented asynchronous cancel hook runs on this thread.
// All normal connection, query and fetch work remains on the owning worker.
class CancelWatch {
    std::mutex mutex_;
    std::condition_variable wake_;
    bool finished_=false;
    std::thread thread_;
public:
    CancelWatch(Cancellation token,std::function<void()> interrupt):thread_([this,token,interrupt]{std::unique_lock<std::mutex> lock(mutex_);while(!finished_){if(token->load()){lock.unlock();interrupt();return;}wake_.wait_for(lock,std::chrono::milliseconds(15));}}){}
    ~CancelWatch(){{std::lock_guard<std::mutex> lock(mutex_);finished_=true;}wake_.notify_one();if(thread_.joinable())thread_.join();}
    CancelWatch(const CancelWatch&)=delete;
    CancelWatch& operator=(const CancelWatch&)=delete;
};
}
