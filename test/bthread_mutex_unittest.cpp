// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>
#include "butil/compat.h"
#include "butil/time.h"
#include "butil/macros.h"
#include "butil/string_printf.h"
#include "butil/logging.h"
#include "bthread/bthread.h"
#include "bthread/butex.h"
#include "bthread/task_control.h"
#include "bthread/mutex.h"
#include "gperftools_helper.h"

namespace {
inline unsigned* get_butex(bthread_mutex_t & m) {
    return m.butex;
}

long start_time = butil::cpuwide_time_ms();
int c = 0;
void* locker(void* arg) {
    bthread_mutex_t* m = (bthread_mutex_t*)arg;
    bthread_mutex_lock(m);
    printf("[%" PRIu64 "] I'm here, %d, %" PRId64 "ms\n", 
           pthread_numeric_id(), ++c, butil::cpuwide_time_ms() - start_time);
    bthread_usleep(10000);
    bthread_mutex_unlock(m);
    return NULL;
}

TEST(MutexTest, sanity) {
    bthread_mutex_t m;
    ASSERT_EQ(0, bthread_mutex_init(&m, NULL));
    ASSERT_EQ(0u, *get_butex(m));
    ASSERT_EQ(0, bthread_mutex_lock(&m));
    ASSERT_EQ(1u, *get_butex(m));
    bthread_t th1;
    ASSERT_EQ(0, bthread_start_urgent(&th1, NULL, locker, &m));
    usleep(5000); // wait for locker to run.
    ASSERT_EQ(257u, *get_butex(m)); // contention
    ASSERT_EQ(0, bthread_mutex_unlock(&m));
    ASSERT_EQ(0, bthread_join(th1, NULL));
    ASSERT_EQ(0u, *get_butex(m));
    ASSERT_EQ(0, bthread_mutex_destroy(&m));
}

TEST(MutexTest, used_in_pthread) {
    bthread_mutex_t m;
    ASSERT_EQ(0, bthread_mutex_init(&m, NULL));
    pthread_t th[8];
    for (size_t i = 0; i < ARRAY_SIZE(th); ++i) {
        ASSERT_EQ(0, pthread_create(&th[i], NULL, locker, &m));
    }
    for (size_t i = 0; i < ARRAY_SIZE(th); ++i) {
        pthread_join(th[i], NULL);
    }
    ASSERT_EQ(0u, *get_butex(m));
    ASSERT_EQ(0, bthread_mutex_destroy(&m));
}

void* do_locks(void *arg) {
    struct timespec t = { -2, 0 };
    EXPECT_EQ(ETIMEDOUT, bthread_mutex_timedlock((bthread_mutex_t*)arg, &t));
    return NULL;
}

TEST(MutexTest, timedlock) {
    bthread_cond_t c;
    bthread_mutex_t m1;
    bthread_mutex_t m2;
    ASSERT_EQ(0, bthread_cond_init(&c, NULL));
    ASSERT_EQ(0, bthread_mutex_init(&m1, NULL));
    ASSERT_EQ(0, bthread_mutex_init(&m2, NULL));

    struct timespec t = { -2, 0 };

    bthread_mutex_lock (&m1);
    bthread_mutex_lock (&m2);
    bthread_t pth;
    ASSERT_EQ(0, bthread_start_urgent(&pth, NULL, do_locks, &m1));
    ASSERT_EQ(ETIMEDOUT, bthread_cond_timedwait(&c, &m2, &t));
    ASSERT_EQ(0, bthread_join(pth, NULL));
    bthread_mutex_unlock(&m1);
    bthread_mutex_unlock(&m2);
    bthread_mutex_destroy(&m1);
    bthread_mutex_destroy(&m2);
}

TEST(MutexTest, cpp_wrapper) {
    bthread::Mutex mutex;
    ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();
    mutex.lock();
    mutex.unlock();
    struct timespec t = { -2, 0 };
    ASSERT_TRUE(mutex.timed_lock(&t));
    mutex.unlock();
    {
        BAIDU_SCOPED_LOCK(mutex);
        ASSERT_FALSE(mutex.timed_lock(&t));
    }
    {
        std::unique_lock<bthread::Mutex> lck1;
        std::unique_lock<bthread::Mutex> lck2(mutex);
        lck1.swap(lck2);
        lck1.unlock();
        lck1.lock();
    }
    ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();
    {
        BAIDU_SCOPED_LOCK(*mutex.native_handler());
    }
    {
        std::unique_lock<bthread_mutex_t> lck1;
        std::unique_lock<bthread_mutex_t> lck2(*mutex.native_handler());
        lck1.swap(lck2);
        lck1.unlock();
        lck1.lock();
    }
    ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();
    ASSERT_TRUE(mutex.timed_lock(&t));
    mutex.unlock();
}

bool g_started = false;
bool g_stopped = false;

template <typename Mutex>
struct BAIDU_CACHELINE_ALIGNMENT PerfArgs {
    Mutex* mutex;
    int64_t counter;
    int64_t elapse_ns;
    bool ready;

    PerfArgs() : mutex(NULL), counter(0), elapse_ns(0), ready(false) {}
};

template <typename Mutex>
void* add_with_mutex(void* void_arg) {
    PerfArgs<Mutex>* args = (PerfArgs<Mutex>*)void_arg;
    args->ready = true;
    butil::Timer t;
    while (!g_stopped) {
        if (g_started) {
            break;
        }
        bthread_usleep(1000);
    }
    t.start();
    while (!g_stopped) {
        BAIDU_SCOPED_LOCK(*args->mutex);
        ++args->counter;
    }
    t.stop();
    args->elapse_ns = t.n_elapsed();
    return NULL;
}

int g_prof_name_counter = 0;

template <typename Mutex, typename ThreadId,
          typename ThreadCreateFn, typename ThreadJoinFn>
void PerfTest(Mutex* mutex,
              ThreadId* /*dummy*/,
              int thread_num,
              const ThreadCreateFn& create_fn,
              const ThreadJoinFn& join_fn) {
    g_started = false;
    g_stopped = false;
    ThreadId threads[thread_num];
    std::vector<PerfArgs<Mutex> > args(thread_num);
    for (int i = 0; i < thread_num; ++i) {
        args[i].mutex = mutex;
        create_fn(&threads[i], NULL, add_with_mutex<Mutex>, &args[i]);
    }
    while (true) {
        bool all_ready = true;
        for (int i = 0; i < thread_num; ++i) {
            if (!args[i].ready) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) {
            break;
        }
        usleep(1000);
    }
    g_started = true;
    char prof_name[32];
    snprintf(prof_name, sizeof(prof_name), "mutex_perf_%d.prof", ++g_prof_name_counter);
    ProfilerStart(prof_name);
    usleep(500 * 1000);
    ProfilerStop();
    g_stopped = true;
    int64_t wait_time = 0;
    int64_t count = 0;
    for (int i = 0; i < thread_num; ++i) {
        join_fn(threads[i], NULL);
        wait_time += args[i].elapse_ns;
        count += args[i].counter;
    }
    LOG(INFO) << butil::class_name<Mutex>() << " in "
              << ((void*)create_fn == (void*)pthread_create ? "pthread" : "bthread")
              << " thread_num=" << thread_num
              << " count=" << count
              << " average_time=" << wait_time / (double)count;
}

TEST(MutexTest, performance) {
    const int thread_num = 12;
    butil::Mutex base_mutex;
    PerfTest(&base_mutex, (pthread_t*)NULL, thread_num, pthread_create, pthread_join);
    PerfTest(&base_mutex, (bthread_t*)NULL, thread_num, bthread_start_background, bthread_join);

    bthread::FastPthreadMutex fast_mutex;
    PerfTest(&fast_mutex, (pthread_t*)NULL, thread_num, pthread_create, pthread_join);
    PerfTest(&fast_mutex, (bthread_t*)NULL, thread_num, bthread_start_background, bthread_join);

    bthread::Mutex bth_mutex;
    PerfTest(&bth_mutex, (pthread_t*)NULL, thread_num, pthread_create, pthread_join);
    PerfTest(&bth_mutex, (bthread_t*)NULL, thread_num, bthread_start_background, bthread_join);
}

template <typename Mutex>
void* loop_until_stopped(void* arg) {
    auto m = (Mutex*)arg;
    while (!g_stopped) {
        BAIDU_SCOPED_LOCK(*m);
        bthread_usleep(20);
    }
    return NULL;
}

TEST(MutexTest, mix_thread_types) {
    g_stopped = false;
    const int N = 16;
    const int M = N * 2;
    bthread::Mutex m;
    pthread_t pthreads[N];
    bthread_t bthreads[M];
    // reserve enough workers for test. This is a must since we have
    // BTHREAD_ATTR_PTHREAD bthreads which may cause deadlocks (the
    // bhtread_usleep below can't be scheduled and g_stopped is never
    // true, thus loop_until_stopped spins forever)
    bthread_setconcurrency(M);
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(0, pthread_create(&pthreads[i], NULL, loop_until_stopped<bthread::Mutex>, &m));
    }
    for (int i = 0; i < M; ++i) {
        const bthread_attr_t *attr = i % 2 ? NULL : &BTHREAD_ATTR_PTHREAD;
        ASSERT_EQ(0, bthread_start_urgent(&bthreads[i], attr, loop_until_stopped<bthread::Mutex>, &m));
    }
    bthread_usleep(1000L * 1000);
    g_stopped = true;
    for (int i = 0; i < M; ++i) {
        bthread_join(bthreads[i], NULL);
    }
    for (int i = 0; i < N; ++i) {
        pthread_join(pthreads[i], NULL);
    }
}

void* do_fast_pthread_timedlock(void *arg) {
    struct timespec t = { -2, 0 };
    EXPECT_FALSE(((bthread::FastPthreadMutex*)arg)->timed_lock(&t));
    EXPECT_EQ(ETIMEDOUT, errno);
    return NULL;
}

TEST(MutexTest, fast_pthread_mutex) {
    bthread::FastPthreadMutex mutex;
    ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();
    mutex.lock();
    mutex.unlock();
    {
        BAIDU_SCOPED_LOCK(mutex);
        struct timespec t = { -2, 0 };
        ASSERT_FALSE(mutex.timed_lock(&t));
        ASSERT_EQ(ETIMEDOUT, errno);
        pthread_t th;
        ASSERT_EQ(0, pthread_create(&th, NULL, do_fast_pthread_timedlock, &mutex));
        ASSERT_EQ(0, pthread_join(th, NULL));
    }
    {
        std::unique_lock<bthread::FastPthreadMutex> lck1;
        std::unique_lock<bthread::FastPthreadMutex> lck2(mutex);
        lck1.swap(lck2);
        lck1.unlock();
        lck1.lock();
    }
    ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();

    const int N = 16;
    pthread_t pthreads[N];
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(0, pthread_create(&pthreads[i], NULL,
            loop_until_stopped<bthread::FastPthreadMutex>, &mutex));
    }
    bthread_usleep(1000L * 1000);
    g_stopped = true;
    for (int i = 0; i < N; ++i) {
        pthread_join(pthreads[i], NULL);
    }
}

#if HAS_PTHREAD_MUTEX_TIMEDLOCK
void* do_pthread_timedlock(void *arg) {
    struct timespec t = { -2, 0 };
    EXPECT_EQ(ETIMEDOUT, pthread_mutex_timedlock((pthread_mutex_t*)arg, &t));
    EXPECT_EQ(ETIMEDOUT, errno);
    return NULL;
}
#endif

TEST(MutexTest, pthread_mutex) {
    pthread_mutex_t mutex;
    ASSERT_EQ(0, pthread_mutex_init(&mutex, NULL));
    ASSERT_EQ(0, pthread_mutex_trylock(&mutex));
    ASSERT_EQ(0, pthread_mutex_unlock(&mutex));
    ASSERT_EQ(0, pthread_mutex_lock(&mutex));
    ASSERT_EQ(0, pthread_mutex_unlock(&mutex));
    {
        BAIDU_SCOPED_LOCK(mutex);
#if HAS_PTHREAD_MUTEX_TIMEDLOCK
        LOG(INFO) << "pthread_mutex_timedlock is available";
        struct timespec t = { -2, 0 };
        ASSERT_EQ(ETIMEDOUT, pthread_mutex_timedlock(&mutex, &t));
        pthread_t th;
        ASSERT_EQ(0, pthread_create(&th, NULL, do_fast_pthread_timedlock, &mutex));
        ASSERT_EQ(0, pthread_join(th, NULL));
#endif
    }
    ASSERT_EQ(0, pthread_mutex_trylock(&mutex));
    ASSERT_EQ(0, pthread_mutex_unlock(&mutex));

    const int N = 16;
    pthread_t pthreads[N];
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(0, pthread_create(&pthreads[i], NULL,
            loop_until_stopped<pthread_mutex_t>, &mutex));
    }
    bthread_usleep(1000L * 1000);
    g_stopped = true;
    for (int i = 0; i < N; ++i) {
        pthread_join(pthreads[i], NULL);
    }
}

} // namespace
