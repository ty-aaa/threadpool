#pragma once
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <utility> // std::forward
namespace mythp{
   






    template <typename F>
    //scope guard类，用于在异常发生时自动调用析构函数，确保资源释放
    class ScopeGuard {
        private:
            F func_;
            bool active_ = true;
        public:
            explicit ScopeGuard(F f):func_(std::move(f)){}
            ~ScopeGuard() {
                if (active_) {
                    func_();
                }
            }
            void dismiss() {
                active_ = false;
            }
            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;
        };  

    
    class ThreadPool {
    public:
        // 构造函数：指定开启多少个工作线程
        explicit ThreadPool(size_t thread_count):stop_(false) {
            if(thread_count == 0)thread_count+=1; // 确保至少有一个线程
            workers_.reserve(thread_count); // 预留空间，避免频繁扩容;
            auto guard = ScopeGuard([this](){shutdown();}); // 使用ScopeGuard确保异常安全
            for (size_t i = 0; i < thread_count; ++i) {
                workers_.emplace_back(&ThreadPool::worker_loop, this); // 创建工作线程并启动
            }
            guard.dismiss(); // 成功创建线程后，取消ScopeGuard的析构调用
        }

        // 析构函数：负责优雅关闭
        ~ThreadPool(){
            shutdown();
        }

        // 提交任务接口（模板函数，支持传入任意函数/lambda以及参数）
        template <typename F,typename... Args>
        void enqueue(F&&f, Args... args){
            enqueue_prio(0,std::forward<F>(f), std::forward<Args>(args)...);
        }
        template <typename F,typename... Args>
        void enqueue_prio(int priority,F&&f, Args... args){
            if(stop_.load()){
                throw std::runtime_error("enqueue on stopped Threadpool");
            }
            auto task = [f = std::forward<F>(f),...a = std::forward<Args>(args)]()mutable{
                std::invoke(std::move(f),std::move(a)...);
            };
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (stop_.load()) throw std::runtime_error("enqueue on stopped ThreadPool");
                tasks_.emplace(pack_task(priority, std::move(task)));
            }
            cv_.notify_one();
        
        }
        
        template <typename... Args>
        void enqueue_batch(int defaultPriority, Args&&... args){
            if(stop_.load()){
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
           std::size_t count = sizeof...(args);
            if(count == 0) return;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if(stop_.load()){
                    throw std::runtime_error("enqueue on stopped ThreadPool");
                }
                // 逗号折叠：对每个参数各调用一次 emplace（必须是一个一个塞，
                // 不能写成 emplace(pack(...)...)，那样会变成 emplace(a,b,c)，参数数不匹配）
                (tasks_.emplace(pack_task(defaultPriority, std::forward<Args>(args))), ...);
            }
            if(count>workers_.size()){
                cv_.notify_all(); // 如果任务数大于线程数，唤醒所有线程
            }else{
                for(size_t i=0;i<count;++i){
                    cv_.notify_one(); // 否则唤醒对应个数线程
                }
            }
        }

    
        void shutdown(){
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (stop_.load()) return;      // ← 幂等检查也放进锁内
                stop_.store(true);             // ★ 状态修改在锁内（唯一的改动）
            }
            cv_.notify_all();                  // 通知在锁外，可以
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
        }
    private:
        // 工作线程的执行体（核心：拿任务、执行任务）
        void worker_loop(){
            while (true) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); }); // 等待任务或关闭信号
                    if (stop_ && tasks_.empty()) {
                        return; // 优雅退出
                    }
                    task = std::move(const_cast<Task&>(tasks_.top())); // 获取任务
                    tasks_.pop();
                }
                try{
                    
                    task.func(); // 执行任务
                    
                } catch (...) {
                    // 处理任务执行异常
                }
            }
        } 
        struct Task {
            std::function<void()> func; // 任务函数
            int priority; // 任务优先级
            std::uint64_t id; // 任务ID

        };
        struct TaskCompare{
            
            bool operator()(const Task& a, const Task& b){
                if (a.priority != b.priority) {
                    return a.priority < b.priority; // 优先级高的任务优先执行
                }
                return a.id > b.id; // 优先级相同，按ID升序
            }
            
        };
        template <typename F>
        Task pack_task(int priority, F&& f) {
            return Task{[f = std::forward<F>(f)]() mutable { std::invoke(std::move(f)); }, priority, task_id_.fetch_add(1) };
        }
        template<typename F>
        Task pack_task(int, std::pair<int, F> &&f) {
            return Task{[f = std::move(f.second)]() mutable { std::invoke(std::move(f)); }, f.first, task_id_.fetch_add(1) };
        }

        // --- 核心成员变量 ---
        std::vector<std::thread> workers_;               // 工作线程组
        std::priority_queue<Task, std::vector<Task>, TaskCompare> tasks_;        // 任务队列（泛型函数包装器）

        std::mutex mtx_;                                 // 保护任务队列的互斥量
        std::condition_variable cv_;                     // 任务队列非空 / 关闭信号的条件变量
        
        std::atomic<bool> stop_;                                      // 优雅关闭标志位
        std::atomic<std::uint64_t> task_id_{0};                           // 任务ID生成器
    };
}