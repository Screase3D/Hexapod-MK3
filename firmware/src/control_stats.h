#pragma once
// Laufzeitstatistik des Regel-Tasks (für die Diagnose in der Oberfläche).

#include <atomic>
#include <cstdint>

namespace control_stats {
inline std::atomic<float> hz{0.0f};             // tatsächliche Zyklusrate
inline std::atomic<uint32_t> max_work_us{0};    // längste Rechenzeit eines Zyklus (letzte Sekunde)
}  // namespace control_stats
