#include "core/video/VideoFrame.h"

#include <chrono>

namespace ks {

qint64 monotonicNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ks
