#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>

// 백오프 알고리즘에 사용될 최소 및 최대 지연 시간 (마이크로초)
#define MIN_DELAY 1
#define MAX_DELAY 1000 

#define START_NUM 1000000
#define END_NUM 5000000

//TASLock
typedef struct TASLock {
    atomic_int state;
} TASLock;

void tas_init_lock(TASLock *lock) {
    atomic_init(&lock->state, 0);
}

void tas_lock(TASLock *lock) {
    while (!__sync_bool_compare_and_swap(&lock->state, 0, 1)) {}
}

void tas_unlock(TASLock *lock) {
    atomic_store(&lock->state, 0);
}

//TTASLock
typedef struct TTASLock {
    atomic_int state;
} TTASLock;

void ttas_init_lock(TTASLock *lock) {
    atomic_init(&lock->state, 0);
}

void ttas_lock(TTASLock *lock) {
    while (1) {
        // 로컬 캐시에서 스핀
        while (atomic_load_explicit(&lock->state, memory_order_relaxed) == 1) {
            // CPU에게 이 스레드가 스핀 중임을 알림
            __builtin_ia32_pause();
        }
        
        // 잠금 시도
        if (atomic_exchange_explicit(&lock->state, 1, memory_order_acquire) == 0) {
            return; // 잠금 획득 성공
        }
    }
}

void ttas_unlock(TTASLock *lock) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
}

//Backofflock
typedef struct{
    atomic_int state;
}BackoffLock;

void backoff_init_lock(BackoffLock *lock) {
    atomic_init(&lock->state, 0);
}

void backoff_lock(BackoffLock* lock){
    int delay = MIN_DELAY;
    while(1){
        // 잠금이 해제될 때까지 대기
        while(atomic_load(&lock->state) == 1){}
        //잠금 시도
        if(__sync_bool_compare_and_swap(&lock->state, 0, 1)){
            return; //성공
        }
        usleep(rand() % delay);
        if(delay < MAX_DELAY){
            delay *= 2;
        }
    }
}

void backoff_unlock(BackoffLock* lock){
    atomic_store(&lock->state, 0);
}

volatile long long result = 0;  // 공유 변수: 결과
volatile int ticket = START_NUM; // 공유 변수: 더할 수
pthread_spinlock_t spinLock; //spinlock 
TASLock tasLock;             // TASLock
TTASLock ttasLock;          //TTASLock
BackoffLock backoffLock;    //backoffLock

/*
worker 함수들
*/

// lock이 없는 경우
void *worker_no_lock(void *arg) {
    int local_ticket;
    while ((local_ticket = ticket++) <= END_NUM) {
        result += local_ticket;
    }
    return NULL;
}

//spin_lock
void* worker_spin_lock(void* arg) {
    int local_ticket;
    while (1) {
        pthread_spin_lock(&spinLock);
        local_ticket = ticket++;
        pthread_spin_unlock(&spinLock);
        
        if (local_ticket > END_NUM) {
            break;
        }
        
        pthread_spin_lock(&spinLock);
        result += local_ticket;
        pthread_spin_unlock(&spinLock);
    }
    return NULL;
}

// tas_lock
void *worker_tas_lock(void *arg) {
    int local_ticket;
    while (1) {
        tas_lock(&tasLock);
        local_ticket = ticket++;
        tas_unlock(&tasLock);
        
        if (local_ticket > END_NUM) {
            break; //루프 종료
        }
        
        tas_lock(&tasLock);
        result += local_ticket;
        tas_unlock(&tasLock);
    }
    return NULL;
}

// ttas_lock
void *worker_ttas_lock(void *arg) {
    int local_ticket;
    while (1) {
        ttas_lock(&ttasLock);
        local_ticket = ticket++;
        ttas_unlock(&ttasLock);
        
        if (local_ticket > END_NUM) {
            break; //루프 종료
        }
        
        ttas_lock(&ttasLock);
        result += local_ticket;
        ttas_unlock(&ttasLock);
    }
    return NULL;
}

// Backoff_lock
void *worker_backoff_lock(void *arg) {
    int local_ticket;
    while (1) {
        backoff_lock(&backoffLock);
        local_ticket = ticket++;
        backoff_unlock(&backoffLock);
        
        if (local_ticket > END_NUM) {
            break; //루프 종료
        }
        
        backoff_lock(&backoffLock);
        result += local_ticket;
        backoff_unlock(&backoffLock);
    }
    return NULL;
}

//한가지 경우의 수를 실험하는 함수
double run_experiment(int num_threads, void* (*worker)(void*)) {
    pthread_t threads[num_threads];
    struct timespec start, end;
    double cpu_time_used = 0;

    // Start time
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Initialize result and ticket number
    result = 0;
    ticket = START_NUM;

    // Create threads
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }

    // Wait for all threads to finish
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // End time
    clock_gettime(CLOCK_MONOTONIC, &end);

    // Calculate time difference in seconds
    cpu_time_used = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
    return cpu_time_used;
}

int main() {
    int thread_counts[] = {2, 4, 6, 8, 10, 12, 14, 16}; // 실험할 스레드 수 배열
    int num_experiments = sizeof(thread_counts) / sizeof(thread_counts[0]);

    // 스핀락 초기화
    pthread_spin_init(&spinLock, 0);

    printf("Lock Type, Threads, Time (s), Result\n");

    // 각 잠금 메커니즘에 대해 실험 수행
    for (int lock = 0; lock < 5; lock++) {
        void* (*worker)(void*);
        const char* lock_name;

        // 현재 실험할 잠금 메커니즘 선택
        switch (lock) {
            case 0:
                worker = worker_no_lock;
                lock_name = "No Lock";
                break;
            case 1:
                worker = worker_spin_lock;
                lock_name = "Spinlock";
                break;
            case 2:
                worker = worker_tas_lock;
                lock_name = "TAS";
                break;
            case 3:
                worker = worker_ttas_lock;
                lock_name = "TTAS";
                break;
            case 4:
                 worker = worker_backoff_lock;
                 lock_name = "Backoff";
                 break;
        }

        // 각 스레드 수에 대해 실험 수행
        for (int i = 0; i < num_experiments; i++) {
            int num_threads = thread_counts[i];
            double time = run_experiment(num_threads, worker);
            printf("lock: %s, num_threads: %d, time: %.6f, result: %lld\n", lock_name, num_threads, time, result);
        }
        printf("\n"); // 각 잠금 메커니즘 실험 후 빈 줄 추가
    }

    // 스핀락 해제
    pthread_spin_destroy(&spinLock);

    return 0;
}



