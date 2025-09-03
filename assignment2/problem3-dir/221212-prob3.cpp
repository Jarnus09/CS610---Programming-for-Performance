#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <immintrin.h>

using std::cerr;
using std::cout;
using std::endl;

using HR = std::chrono::high_resolution_clock;
using HRTimer = HR::time_point;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;

#define N (1e6)
#define NUM_THREADS (1)
#define MAX_NUM_THREADS (1)

// Custom CAS
inline bool cas16(uint16_t &target, uint16_t expected, uint16_t desired)
{
  bool success;
  __asm__ __volatile__(
      "lock cmpxchgw %3, %0\n\t"
      "sete %1"
      : "+m"(target), "=q"(success), "+a"(expected)
      : "r"(desired)
      : "memory");
  return success;
}

// Custom increment
inline uint32_t atomic_inc(volatile uint32_t &target)
{
  uint32_t old_value = 1;
  __asm__ __volatile__(
      "lock xaddl %0, %1"
      : "+r"(old_value), "+m"(target)
      :
      : "memory");

  return old_value;
}

// Shared variables
uint64_t var1 = 0, var2 = (N * NUM_THREADS + 1);

// Abstract base class
class LockBase
{
public:
  // Pure virtual function
  virtual void acquire(uint16_t tid) = 0;
  virtual void release(uint16_t tid) = 0;
};

typedef struct thr_args
{
  uint16_t m_id;
  LockBase *m_lock;
} ThreadArgs;

/** Use pthread mutex to implement lock routines */
class PthreadMutex : public LockBase
{
public:
  void acquire(uint16_t tid) override { pthread_mutex_lock(&lock); }
  void release(uint16_t tid) override { pthread_mutex_unlock(&lock); }

private:
  pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
};

class FilterLock : public LockBase
{
  volatile uint32_t level[MAX_NUM_THREADS] = {0};
  volatile uint32_t victim[MAX_NUM_THREADS] = {0};

public:
  void acquire(uint16_t tid) override
  {
    for (uint32_t i = 1; i < MAX_NUM_THREADS; i++)
    {
      level[tid] = i;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      victim[i] = tid;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      while (1)
      {
        if (victim[i] != tid)
          break;
        bool found = false;
        for (uint32_t k = 0; k < MAX_NUM_THREADS; k++)
        {
          if (k == tid)
            continue;
          if (level[k] >= i)
          {
            found = true;
            break;
          }
        }
        if (!found)
          break;
      }
    }
  }
  void release(uint16_t tid) override
  {
    assert(level[tid] == MAX_NUM_THREADS - 1);
    level[tid] = 0;
  }

  FilterLock() {}
  ~FilterLock() {}
};

class BakeryLock : public LockBase
{
  volatile uint32_t choosing[MAX_NUM_THREADS] = {0};
  volatile uint32_t label[MAX_NUM_THREADS] = {0};
  uint32_t max_label = 0;

public:
  void acquire(uint16_t tid) override
  {
    choosing[tid] = 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    label[tid] = ++max_label;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    while (1)
    {
      bool found = false;
      for (uint32_t k = 0; k < MAX_NUM_THREADS; k++)
      {
        if (k == tid)
          continue;
        if (choosing[k])
          if (label[k] != 0 && (label[k] < label[tid] || (label[k] == label[tid] && k < tid)))
          {
            found = true;
            break;
          }
      }
      if (!found)
        break;
    }
  }
  void release(uint16_t tid) override
  {
    choosing[tid] = 0;
  }

  BakeryLock() {}
  ~BakeryLock() {}
};

class SpinLock : public LockBase
{
private:
  uint16_t lock_taken = 0;
  const uint16_t UNLOCKED = 0;
  const uint16_t LOCKED = 1;

public:
  void acquire(uint16_t tid) override
  {
    while (!cas16(lock_taken, UNLOCKED, LOCKED))
      ;
  }
  void release(uint16_t tid) override
  {
    cas16(lock_taken, LOCKED, UNLOCKED);
  }

  SpinLock() {}
  ~SpinLock() {}
};

class TicketLock : public LockBase
{
private:
  volatile uint32_t next_ticket = 0;
  volatile uint32_t serving_ticket = 0;

public:
  void acquire(uint16_t tid) override
  {
    uint32_t my_ticket = atomic_inc(next_ticket);

    while (my_ticket != serving_ticket)
      ;
  }
  void release(uint16_t tid) override
  {
    serving_ticket++;
  }

  TicketLock() {}
  ~TicketLock() {}
};

class ArrayQLock : public LockBase
{
  uint32_t next_avail_id = 0;
  volatile uint32_t queue[MAX_NUM_THREADS] = {0};
  uint32_t queue_id[MAX_NUM_THREADS] = {0};

public:
  void acquire(uint16_t tid) override
  {
    uint32_t my_queue_id = (atomic_inc(next_avail_id));

    if (my_queue_id == 0) // At the beginning
      queue[my_queue_id] = 1;

    my_queue_id = my_queue_id % MAX_NUM_THREADS;
    queue_id[tid] = my_queue_id;

    while (queue[my_queue_id] == 0)
      ;
  }
  void release(uint16_t tid) override
  {
    uint32_t my_queue_id = queue_id[tid];
    uint32_t next_queue_id = (my_queue_id + 1) % MAX_NUM_THREADS;
    queue[my_queue_id] = 0;
    queue[next_queue_id] = 1;
  }

  ArrayQLock() {}
  ~ArrayQLock() {}
};

/** Estimate the time taken */
std::atomic_uint64_t sync_time = 0;

inline void critical_section()
{
  var1++;
  var2--;
}

/** Sync threads at the start to maximize contention */
pthread_barrier_t g_barrier;

void *thrBody(void *arguments)
{
  ThreadArgs *tmp = static_cast<ThreadArgs *>(arguments);
  if (false)
  {
    cout << "Thread id: " << tmp->m_id << " starting\n";
  }

  // Wait for all other producer threads to launch before proceeding.
  pthread_barrier_wait(&g_barrier);

  HRTimer start = HR::now();
  for (int i = 0; i < N; i++)
  {
    tmp->m_lock->acquire(tmp->m_id);
    critical_section();
    tmp->m_lock->release(tmp->m_id);
  }
  HRTimer end = HR::now();
  auto duration = duration_cast<milliseconds>(end - start).count();

  // A barrier is not required here
  sync_time.fetch_add(duration);
  pthread_exit(NULL);
}

int main()
{
  int error = pthread_barrier_init(&g_barrier, NULL, NUM_THREADS);
  if (error != 0)
  {
    cerr << "Error in barrier init.\n";
    exit(EXIT_FAILURE);
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  pthread_t tid[NUM_THREADS];
  ThreadArgs args[NUM_THREADS] = {{0}};

  // Pthread mutex
  LockBase *lock_obj = new PthreadMutex();
  uint16_t i = 0;
  while (i < NUM_THREADS)
  {
    args[i].m_id = i;
    args[i].m_lock = lock_obj;

    error = pthread_create(&tid[i], &attr, thrBody, (void *)(args + i));
    if (error != 0)
    {
      cerr << "\nThread cannot be created : " << strerror(error) << "\n";
      exit(EXIT_FAILURE);
    }
    i++;
  }

  i = 0;
  void *status;
  while (i < NUM_THREADS)
  {
    error = pthread_join(tid[i], &status);
    if (error)
    {
      cerr << "ERROR: return code from pthread_join() is " << error << "\n";
      exit(EXIT_FAILURE);
    }
    i++;
  }

  assert(var1 == N * NUM_THREADS && var2 == 1);
  cout << "Var1: " << var1 << "\tVar2: " << var2 << "\n";
  cout << "Pthread mutex: Time taken (us): " << sync_time << "\n";

  // Filter lock
  var1 = 0;
  var2 = (N * NUM_THREADS + 1);
  sync_time.store(0);

  lock_obj = new FilterLock();
  i = 0;
  while (i < NUM_THREADS)
  {
    args[i].m_id = i;
    args[i].m_lock = lock_obj;

    error = pthread_create(&tid[i], &attr, thrBody, (void *)(args + i));
    if (error != 0)
    {
      printf("\nThread cannot be created : [%s]", strerror(error));
      exit(EXIT_FAILURE);
    }
    i++;
  }

  i = 0;
  while (i < NUM_THREADS)
  {
    error = pthread_join(tid[i], &status);
    if (error)
    {
      printf("ERROR: return code from pthread_join() is %d\n", error);
      exit(EXIT_FAILURE);
    }
    i++;
  }

  cout << "Var1: " << var1 << "\tVar2: " << var2 << "\n";
  assert(var1 == N * NUM_THREADS && var2 == 1);
  cout << "Filter lock: Time taken (us): " << sync_time << "\n";

  // Bakery lock
  var1 = 0;
  var2 = (N * NUM_THREADS + 1);
  sync_time.store(0);

  lock_obj = new BakeryLock();
  i = 0;
  while (i < NUM_THREADS)
  {
    args[i].m_id = i;
    args[i].m_lock = lock_obj;

    error = pthread_create(&tid[i], &attr, thrBody, (void *)(args + i));
    if (error != 0)
    {
      printf("\nThread cannot be created : [%s]", strerror(error));
      exit(EXIT_FAILURE);
    }
    i++;
  }

  i = 0;
  while (i < NUM_THREADS)
  {
    error = pthread_join(tid[i], &status);
    if (error)
    {
      printf("ERROR: return code from pthread_join() is %d\n", error);
      exit(EXIT_FAILURE);
    }
    i++;
  }

  cout << "Var1: " << var1 << "\tVar2: " << var2 << "\n";
  assert(var1 == N * NUM_THREADS && var2 == 1);
  cout << "Bakery lock: Time taken (us): " << sync_time << "\n";

  // Spin lock
  var1 = 0;
  var2 = (N * NUM_THREADS + 1);
  sync_time.store(0);

  lock_obj = new SpinLock();
  i = 0;
  while (i < NUM_THREADS)
  {
    args[i].m_id = i;
    args[i].m_lock = lock_obj;

    error = pthread_create(&tid[i], &attr, thrBody, (void *)(args + i));
    if (error != 0)
    {
      printf("\nThread cannot be created : [%s]", strerror(error));
      exit(EXIT_FAILURE);
    }
    i++;
  }

  i = 0;
  while (i < NUM_THREADS)
  {
    error = pthread_join(tid[i], &status);
    if (error)
    {
      printf("ERROR: return code from pthread_join() is %d\n", error);
      exit(EXIT_FAILURE);
    }
    i++;
  }

  cout << "Var1: " << var1 << "\tVar2: " << var2 << "\n";
  assert(var1 == N * NUM_THREADS && var2 == 1);
  cout << "Spin lock: Time taken (us): " << sync_time << "\n";

  // Ticket lock
  var1 = 0;
  var2 = (N * NUM_THREADS + 1);
  sync_time.store(0);

  lock_obj = new TicketLock();
  i = 0;
  while (i < NUM_THREADS)
  {
    args[i].m_id = i;
    args[i].m_lock = lock_obj;

    error = pthread_create(&tid[i], &attr, thrBody, (void *)(args + i));
    if (error != 0)
    {
      printf("\nThread cannot be created : [%s]", strerror(error));
      exit(EXIT_FAILURE);
    }
    i++;
  }

  i = 0;
  while (i < NUM_THREADS)
  {
    error = pthread_join(tid[i], &status);
    if (error)
    {
      printf("ERROR: return code from pthread_join() is %d\n", error);
      exit(EXIT_FAILURE);
    }
    i++;
  }

  cout << "Var1: " << var1 << "\tVar2: " << var2 << "\n";
  assert(var1 == N * NUM_THREADS && var2 == 1);
  cout << "Ticket lock: Time taken (us): " << sync_time << "\n";

  // Array Q lock
  var1 = 0;
  var2 = (N * NUM_THREADS + 1);
  sync_time.store(0);

  lock_obj = new ArrayQLock();
  i = 0;
  while (i < NUM_THREADS)
  {
    args[i].m_id = i;
    args[i].m_lock = lock_obj;

    error = pthread_create(&tid[i], &attr, thrBody, (void *)(args + i));
    if (error != 0)
    {
      printf("\nThread cannot be created : [%s]", strerror(error));
      exit(EXIT_FAILURE);
    }
    i++;
  }

  i = 0;
  while (i < NUM_THREADS)
  {
    error = pthread_join(tid[i], &status);
    if (error)
    {
      printf("ERROR: return code from pthread_join() is %d\n", error);
      exit(EXIT_FAILURE);
    }
    i++;
  }

  cout << "Var1: " << var1 << "\tVar2: " << var2 << "\n";
  assert(var1 == N * NUM_THREADS && var2 == 1);
  cout << "Array Q lock: Time taken (us): " << sync_time << "\n";

  pthread_barrier_destroy(&g_barrier);
  pthread_attr_destroy(&attr);

  pthread_exit(NULL);
}

