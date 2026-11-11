// 演示"虚假唤醒"（spurious wakeup）
//
// 真实硬件上，cv.wait() 有时会在【没有人调用 notify】的情况下返回。
// 这是允许的 —— 标准明确说 condition_variable 存在虚假唤醒。
//
// 除非你有特殊硬件，否则很难稳定复现真实的虚假唤醒。
// 所以我们用一个"假通知器"来模拟它的【效果】：
//   在不该通知的时候 notify 一下 —— 对消费者来说，和硬件虚假唤醒完全等价。
//
// 跑法：
//   g++ -std=c++20 -pthread demo_spurious_wakeup.cpp -o demo && ./demo

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>

using namespace std::chrono_literals;

namespace {
std::mutex m;
std::condition_variable cv;
std::queue<int> tasks;
}

// ══════════════════════════════════════════════════════════════
// ❌ 错误版：裸 wait（没有谓词）
// ══════════════════════════════════════════════════════════════
void bad_consumer(bool* woke_with_empty_queue) {
    std::unique_lock<std::mutex> lk(m);

    std::printf("  [消费者] 准备 wait...\n");
    cv.wait(lk);                          // ← 醒来就往下走，不管条件成不成立

    // 真实代码在这里会直接写：
    //     int x = tasks.front();  tasks.pop();
    // 队列空的时候 front() 是【未定义行为】—— 可能崩溃，可能读到垃圾。
    if (tasks.empty()) {
        *woke_with_empty_queue = true;    // 演示版：只做检测，不真去 front()
        std::printf("  [消费者] 💥 我醒了，但队列是空的！\n");
        std::printf("  [消费者] 真实代码在这里就 UB 了（front() 空队列）\n");
    } else {
        std::printf("  [消费者] 正常拿到任务 %d\n", tasks.front());
        tasks.pop();
    }
}

// ══════════════════════════════════════════════════════════════
// ✅ 正确版：谓词循环
// ══════════════════════════════════════════════════════════════
void good_consumer() {
    std::unique_lock<std::mutex> lk(m);

    std::printf("  [消费者] 准备 wait...\n");

    // 谓词循环，等价于：
    //     while (tasks.empty()) cv.wait(lk);
    //
    // 醒来后第一件事是【重新检查条件】——
    // 条件不成立就继续睡，根本不管是谁把它叫醒的。
    cv.wait(lk, [] { return !tasks.empty(); });

    std::printf("  [消费者] ✅ 醒来且队列非空，安全拿到任务 %d\n", tasks.front());
    tasks.pop();
}

// ══════════════════════════════════════════════════════════════
// 假通知器：条件不成立时也发通知
// ══════════════════════════════════════════════════════════════
void fake_notifier(const char* who) {
    std::this_thread::sleep_for(100ms);   // 等消费者真的进入 wait
    {
        std::lock_guard<std::mutex> lk(m); // 加锁只是为了不改动条件
        std::printf("  [%s] 队列里还是空的，但我要 notify 了（模拟虚假唤醒）\n", who);
    }
    cv.notify_one();                      // ← 条件没变，纯通知
}

int main() {
    // ══════════════════════════════════════════════════════
    std::puts("════════ 场景 1：❌ 裸 wait ════════");
    // ══════════════════════════════════════════════════════
    {
        bool woke_empty = false;
        std::thread consumer(bad_consumer, &woke_empty);
        std::thread notifier(fake_notifier, "假通知器");
        consumer.join();
        notifier.join();

        std::puts(woke_empty ? "  → 结论：裸 wait 被骗了，条件不成立也往下走 ❌\n"
                             : "  → 这次没被骗（时序巧合）\n");
        // 清一下队列
        std::lock_guard<std::mutex> lk(m);
        while (!tasks.empty()) tasks.pop();
    }

    // ══════════════════════════════════════════════════════
    std::puts("════════ 场景 2：✅ 谓词 wait，先假通知再真通知 ════════");
    // ══════════════════════════════════════════════════════
    {
        std::thread consumer(good_consumer);

        // ① 先来一次假通知 —— 谓词会拦住它
        fake_notifier("假通知器");

        std::this_thread::sleep_for(100ms);

        // ② 再来一次真通知 —— 这次条件真的成立了
        {
            std::lock_guard<std::mutex> lk(m);
            tasks.push(42);
            std::printf("  [真生产者] 放入任务 42\n");
        }
        cv.notify_one();

        consumer.join();
        std::puts("  → 结论：假通知被谓词挡掉，真任务照常处理 ✅\n");
    }

    // ══════════════════════════════════════════════════════
    std::puts("════════ 本质 ════════");
    std::puts("  cv.wait(lk)        被唤醒 ≠ 条件成立");
    std::puts("  cv.wait(lk, pred)  被唤醒后【必须重新检查】，不成立就继续睡");
    std::puts("");
    std::puts("  等价写法：");
    std::puts("      while (!pred()) cv.wait(lk);");
    std::puts("");
    std::puts("  线程池里你的写法：");
    std::puts("      cv_.wait(lock, [this]{ return stop_ || !tasks_.empty(); });");
    std::puts("  —— 这就是为什么它不会从空队列取任务。");

    return 0;
}
