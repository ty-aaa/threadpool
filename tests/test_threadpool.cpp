// ThreadPool 的单元测试（W5）
//
// 和前几个项目最大的不同：
//   前面测"结果对不对"，这里还要测"**能不能优雅退出**""**会不会卡死**"。
//
// 验收标准：**TSan 干净**
//   cmake -B build-tsan -G Ninja -DENABLE_TSAN=ON
//   cmake --build build-tsan && ./build-tsan/test_threadpool
//
// 【已知限制】v0 不支持仅可移动的任务 —— 任务被包进 std::function，
//           而 std::function 要求目标可拷贝。这是 W6 v1 要解决的。

#include "mythp/Threadpool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ══════════════════════════════════════════════════════════════
// 1. 基本功能
// ══════════════════════════════════════════════════════════════

TEST(ThreadPool, ExecutesTask) {
    std::atomic<int> ran{0};
    {
        mythp::ThreadPool pool(2);
        pool.enqueue([&ran] { ran.fetch_add(1); });
    }
    EXPECT_EQ(ran.load(), 1);
}

TEST(ThreadPool, ExecutesWithArguments) {
    std::atomic<int> sum{0};
    {
        mythp::ThreadPool pool(2);
        for (int i = 1; i <= 100; ++i) {
            pool.enqueue([&sum](int a, int b) { sum.fetch_add(a + b); }, i, i);
        }
    }
    EXPECT_EQ(sum.load(), 10100);
}

// ══════════════════════════════════════════════════════════════
// 2. 并发度：线程数真的用上了吗
// ══════════════════════════════════════════════════════════════

TEST(ThreadPool, RunsOnAllWorkers) {
    constexpr int kThreads = 4;
    std::atomic<int> running{0};
    std::atomic<int> peak{0};

    {
        mythp::ThreadPool pool(kThreads);
        for (int i = 0; i < kThreads * 4; ++i) {
            pool.enqueue([&] {
                int now = running.fetch_add(1) + 1;
                int prev = peak.load();
                while (now > prev && !peak.compare_exchange_weak(prev, now)) {
                }
                std::this_thread::sleep_for(30ms);
                running.fetch_sub(1);
            });
        }
    }

    EXPECT_EQ(peak.load(), kThreads);
}

TEST(ThreadPool, SingleThreadIsSerial) {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};

    {
        mythp::ThreadPool pool(1);
        for (int i = 0; i < 8; ++i) {
            pool.enqueue([&] {
                int now = running.fetch_add(1) + 1;
                int prev = peak.load();
                while (now > prev && !peak.compare_exchange_weak(prev, now)) {
                }
                std::this_thread::sleep_for(10ms);
                running.fetch_sub(1);
            });
        }
    }

    EXPECT_EQ(peak.load(), 1);
}

// ══════════════════════════════════════════════════════════════
// 3. 无丢失（W5 验收：100 万任务）
// ══════════════════════════════════════════════════════════════

TEST(ThreadPool, MillionTasksNoLoss) {
    constexpr int kTasks = 1'000'000;
    std::atomic<long> counter{0};

    {
        mythp::ThreadPool pool(4);
        for (int i = 0; i < kTasks; ++i) {
            pool.enqueue([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }

    EXPECT_EQ(counter.load(), kTasks) << "任务丢失 —— 关闭时没把队列排空";
}

// ══════════════════════════════════════════════════════════════
// 4. 优雅关闭
// ══════════════════════════════════════════════════════════════

// 析构时队列里还有任务 → 必须全部做完再退出
TEST(ThreadPool, DrainsQueueOnShutdown) {
    std::atomic<int> done{0};
    {
        mythp::ThreadPool pool(2);
        for (int i = 0; i < 500; ++i) {
            pool.enqueue([&done] {
                std::this_thread::sleep_for(1ms);
                done.fetch_add(1);
            });
        }
    }
    EXPECT_EQ(done.load(), 500) << "关闭时丢弃了未执行的任务";
}

// 空池子销毁不该卡住
TEST(ThreadPool, EmptyPoolDestroysQuickly) {
    auto t0 = std::chrono::steady_clock::now();
    {
        mythp::ThreadPool pool(4);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 2000);
}

// ══════════════════════════════════════════════════════════════
// 5. 异常隔离
// ══════════════════════════════════════════════════════════════

TEST(ThreadPool, TaskExceptionDoesNotKillWorker) {
    std::atomic<int> ok{0};
    {
        mythp::ThreadPool pool(2);
        pool.enqueue([] { throw std::runtime_error("boom"); });
        for (int i = 0; i < 200; ++i) {
            pool.enqueue([&ok] { ok.fetch_add(1); });
        }
    }
    EXPECT_EQ(ok.load(), 200);
}

// ══════════════════════════════════════════════════════════════
// 6. 并发压力
// ══════════════════════════════════════════════════════════════

// 多个生产者同时提交
TEST(ThreadPool, ConcurrentEnqueue) {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 10'000;
    std::atomic<long> counter{0};

    {
        mythp::ThreadPool pool(4);
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&pool, &counter] {
                for (int i = 0; i < kPerProducer; ++i) {
                    pool.enqueue([&counter] {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    });
                }
            });
        }
        for (auto& t : producers) t.join();
    }

    EXPECT_EQ(counter.load(), static_cast<long>(kProducers) * kPerProducer);
}

// 生产者和消费者同时活跃 —— 最容易出竞争的场景
TEST(ThreadPool, ProducerConsumerOverlap) {
    std::atomic<long> produced{0};
    std::atomic<long> consumed{0};

    {
        mythp::ThreadPool pool(4);
        std::thread producer([&] {
            for (int i = 0; i < 50'000; ++i) {
                produced.fetch_add(1, std::memory_order_relaxed);
                pool.enqueue([&consumed] {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
        producer.join();
    }

    EXPECT_EQ(produced.load(), 50'000);
    EXPECT_EQ(consumed.load(), 50'000);
}

// ══════════════════════════════════════════════════════════════
// 7. 参数正确性：每个任务拿到的必须是它自己的参数
// ══════════════════════════════════════════════════════════════

TEST(ThreadPool, ArgumentsAreNotMixed) {
    constexpr int kTasks = 1000;
    std::vector<std::atomic<int>> seen(kTasks);
    for (auto& x : seen) x.store(-1);

    {
        mythp::ThreadPool pool(4);
        for (int i = 0; i < kTasks; ++i) {
            pool.enqueue([&seen](int idx) { seen[idx].store(idx); }, i);
        }
    }

    for (int i = 0; i < kTasks; ++i) {
        EXPECT_EQ(seen[i].load(), i) << "第 " << i << " 个任务的参数串了";
    }
}

// ══════════════════════════════════════════════════════════════
// 8. 优先级（L3 新增）
//
// 测法：单线程池 + 一个"阻塞器"任务把唯一的线程占住，
// 这样后面提交的任务一定全部排在队列里，执行顺序才可断言。
// ══════════════════════════════════════════════════════════════

TEST(ThreadPoolPriority, HigherPriorityRunsFirst) {
    std::vector<int> order;
    std::mutex order_mtx;
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};

    {
        mythp::ThreadPool pool(1);

        // 阻塞器：占住唯一的线程
        pool.enqueue([&] {
            started.store(true);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // 现在线程被堵住，这三个一定排队等
        pool.enqueue_prio(1, [&] {
            std::lock_guard<std::mutex> lk(order_mtx);
            order.push_back(1);
        });
        pool.enqueue_prio(3, [&] {
            std::lock_guard<std::mutex> lk(order_mtx);
            order.push_back(3);
        });
        pool.enqueue_prio(2, [&] {
            std::lock_guard<std::mutex> lk(order_mtx);
            order.push_back(2);
        });

        release.store(true, std::memory_order_release);
    }

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 3) << "最高优先级的应该第一个执行";
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 1) << "最低优先级的应该最后执行";
}

// 同优先级必须按提交顺序（先进先出），否则相同优先级的任务会乱序
TEST(ThreadPoolPriority, SamePriorityIsFifo) {
    std::vector<int> order;
    std::mutex order_mtx;
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};

    {
        mythp::ThreadPool pool(1);
        pool.enqueue([&] {
            started.store(true);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();

        for (int i = 0; i < 5; ++i) {
            pool.enqueue_prio(0, [&, i] {
                std::lock_guard<std::mutex> lk(order_mtx);
                order.push_back(i);
            });
        }
        release.store(true, std::memory_order_release);
    }

    ASSERT_EQ(order.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(order[i], i) << "同优先级没按提交顺序执行";
    }
}

// 默认 enqueue 走优先级 0，应该排在有优先级的任务之后
TEST(ThreadPoolPriority, DefaultEnqueueIsLowestAmongPrios) {
    std::vector<int> order;
    std::mutex order_mtx;
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};

    {
        mythp::ThreadPool pool(1);
        pool.enqueue([&] {
            started.store(true);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();

        pool.enqueue([&] {                    // 默认优先级 0
            std::lock_guard<std::mutex> lk(order_mtx);
            order.push_back(0);
        });
        pool.enqueue_prio(1, [&] {
            std::lock_guard<std::mutex> lk(order_mtx);
            order.push_back(1);
        });

        release.store(true, std::memory_order_release);
    }

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 0);
}

// ══════════════════════════════════════════════════════════════
// 9. 批量提交（L3 新增）
// ══════════════════════════════════════════════════════════════

TEST(ThreadPoolBatch, EnqueuesAllTasks) {
    std::atomic<int> n{0};
    {
        mythp::ThreadPool pool(4);
        pool.enqueue_batch(0,
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); });
    }
    EXPECT_EQ(n.load(), 3);
}

// 任务数 > 线程数时走 notify_all 分支
TEST(ThreadPoolBatch, MoreTasksThanWorkers) {
    std::atomic<int> n{0};
    {
        mythp::ThreadPool pool(2);
        pool.enqueue_batch(0,
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); },
                           [&] { n.fetch_add(1); });
    }
    EXPECT_EQ(n.load(), 8);
}

// 批量提交也可以带优先级
TEST(ThreadPoolBatch, BatchWithPriorityRunsBeforeDefault) {
    std::vector<int> order;
    std::mutex order_mtx;
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};

    {
        mythp::ThreadPool pool(1);
        pool.enqueue([&] {
            started.store(true);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();

        pool.enqueue([&] {                    // 优先级 0
            std::lock_guard<std::mutex> lk(order_mtx);
            order.push_back(0);
        });
        pool.enqueue_batch(5,                 // 优先级 5
                           [&] {
                               std::lock_guard<std::mutex> lk(order_mtx);
                               order.push_back(5);
                           },
                           [&] {
                               std::lock_guard<std::mutex> lk(order_mtx);
                               order.push_back(5);
                           });

        release.store(true, std::memory_order_release);
    }

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 5);
    EXPECT_EQ(order[1], 5);
    EXPECT_EQ(order[2], 0) << "默认优先级的应该最后";
}

// 空批量提交应当无副作用
TEST(ThreadPoolBatch, EmptyBatchIsNoOp) {
    std::atomic<int> n{0};
    {
        mythp::ThreadPool pool(2);
        pool.enqueue_batch(0);                // 零个任务
        pool.enqueue([&] { n.fetch_add(1); });
    }
    EXPECT_EQ(n.load(), 1);
}

// 带参数的任务（优先级版）
TEST(ThreadPoolPriority, BindsArguments) {
    std::atomic<int> sum{0};
    {
        mythp::ThreadPool pool(2);
        pool.enqueue_prio(3, [&sum](int a, int b) { sum.fetch_add(a + b); }, 20, 22);
    }
    EXPECT_EQ(sum.load(), 42);
}
