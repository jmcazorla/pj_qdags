#ifndef QDAGS_STRATEGY_STRATEGY_RACE_HPP
#define QDAGS_STRATEGY_STRATEGY_RACE_HPP

#include <functional>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdint>

#include "../join/joins.hpp"

// raceStrategies corre dos callables (la misma estrategia con parallel_level=0 y con >0), uno por thread, y se
// queda con el del que termine primero, cancelando al otro. Asi no hay que decidir de antemano si conviene
// paralelizar. Limitaciones: un thread de mas durante la carrera y el costo de lanzar ambas ramas.

namespace gdj {

    struct RaceResult {
        uint64_t value = 0;
        int winner = -1;        // 0 = seq_fn gano, 1 = par_fn gano
        double seq_time = -1.0; // tiempo de fin de seq_fn (mayor al de la carrera si se aborto tarde)
        double par_time = -1.0; // idem para par_fn
    };

    // Corre seq_fn y par_fn concurrentemente y devuelve el resultado de la que termine primero. Ambas deben liberar
    // sus recursos antes de retornar, tambien si abortan. time_out > 0 arma ademas el watchdog, que puede abortar
    // ambas ramas.
    inline RaceResult raceStrategies(std::function<uint64_t()> seq_fn,
                                      std::function<uint64_t()> par_fn,
                                      uint64_t time_out = 0) {
        std::mutex m;
        bool done = false;
        RaceResult out;

        start_timeout_watchdog(time_out);

        auto run = [&](int id, std::function<uint64_t()>& fn, double* out_time) {
            auto t0 = std::chrono::high_resolution_clock::now();
            uint64_t r = fn();
            auto t1 = std::chrono::high_resolution_clock::now();
            *out_time = std::chrono::duration<double>(t1 - t0).count();

            std::lock_guard<std::mutex> lock(m);
            if (!done) {
                done = true;
                out.winner = id;
                out.value = r;
            }
            // Se senaliza siempre, no solo el ganador: si esta rama termino por timeout, la otra tambien aborta.
            timeout_triggered.store(true, std::memory_order_relaxed);
        };

        std::thread t_seq([&]{ run(0, seq_fn, &out.seq_time); });
        std::thread t_par([&]{ run(1, par_fn, &out.par_time); });
        t_seq.join();
        t_par.join();

        stop_timeout_watchdog();

        // Sin este reset, el proximo join (aunque no pase por raceStrategies) abortaria de inmediato.
        timeout_triggered.store(false, std::memory_order_relaxed);

        return out;
    }

} // namespace gdj

#endif // QDAGS_STRATEGY_STRATEGY_RACE_HPP
