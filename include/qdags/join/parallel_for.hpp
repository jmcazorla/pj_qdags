#ifndef QDAGS_PARALLEL_FOR_HPP
#define QDAGS_PARALLEL_FOR_HPP

#include <algorithm>
#include <thread>
#include <functional>
#include <vector>
#include <mutex>
#include <iostream>
#include <pthread.h>
#include <sched.h>

// parallel_for: reparte [0, nb_elements) en bloques contiguos entre hardware_concurrency()*THREADS_BY_CORE hilos,
// con afinidad de CPU por hilo. Solo Linux (pthread_setaffinity_np).

#ifndef THREADS_BY_CORE
#define THREADS_BY_CORE 1
#endif

// functor(start, end) procesa el sub-rango [start, end); use_threads=false lo corre en el hilo actual.
static
void parallel_for(unsigned nb_elements,
                  std::function<void (int start, int end)> functor,
                  bool use_threads = true) {
    unsigned nb_threads_hint = THREADS_BY_CORE * std::thread::hardware_concurrency();
    unsigned nb_threads = nb_threads_hint == 0 ? 8 : (nb_threads_hint);

    unsigned batch_size = nb_elements / nb_threads;
    unsigned batch_remainder = nb_elements % nb_threads;

    std::vector< std::thread > my_threads(nb_threads);

    if( use_threads ) {
        for(unsigned i = 0; i < nb_threads; ++i) {
            int start = i * batch_size;
            my_threads[i] = std::thread(functor, start, start+batch_size);

            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(i / THREADS_BY_CORE, &cpuset);
            int rc = pthread_setaffinity_np(my_threads[i].native_handle(),
                                            sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
            }
        }
    }
    else {
        for(unsigned i = 0; i < nb_threads; ++i){
            int start = i * batch_size;
            functor( start, start+batch_size );
        }
    }

    int start = nb_threads * batch_size;
    functor( start, start+batch_remainder);

    if( use_threads )
        std::for_each(my_threads.begin(), my_threads.end(), std::mem_fn(&std::thread::join));
}

#endif // QDAGS_PARALLEL_FOR_HPP
