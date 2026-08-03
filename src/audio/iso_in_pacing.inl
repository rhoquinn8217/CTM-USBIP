// iso_in_pacing.inl -- FORK-ONLY (rhoquinn8217/CTM-USBIP).
//
// Paces inbound isochronous (microphone) URB completions to real time.
//
// WHY THIS EXISTS
//   CtmUsbipDevice::handle_endpoint_in() answers an audio IN request
//   immediately with a zero-filled buffer of the requested length. The
//   Windows USB audio driver resubmits as soon as it is answered, so the
//   microphone endpoint free-runs. Measured on the wired DS5, 2026-08-02:
//   ep82_in_hz = 27733 and 28285, against the ~1000/second a real 1 ms
//   isochronous IN endpoint produces. That is an accidental busy loop: it
//   burns CPU, and the completions carry no timing relationship to real
//   audio, so the host's capture clock never advances (Windows Sound
//   Recorder shows a stream that opens but whose timer stays at 00:00:00).
//
// WHY NOT INSIDE THE HANDLER
//   Audio IN requests are answered INLINE on the URB read loop
//   (CtmUsbipServer::urb_loop). Interrupt IN is offloaded to per-endpoint
//   worker threads; isochronous IN is not. Sleeping inside the handler
//   therefore stalls the loop that reads EVERY URB, including HID input at
//   250 Hz. The completion has to be deferred instead.
//
// PROVENANCE (Fork Design Rule 1)
//   The queue + worker + sleep_until pacing shape is COPIED from the
//   outbound acknowledgement worker `isoOutAckWorker` in
//   src/usbip/server.inl of this same repo, at commit 4fdf97f. It is
//   duplicated here rather than refactored into something both directions
//   share, per Rule 1: upstream's code is called, not restructured.
//   Differences from the original, deliberately: no reservoir low-water
//   rule (there is no inbound reservoir), and no rate regulator (nothing
//   inbound to regulate against yet).
//
// SCOPE
//   This file paces completions ONLY. It does not put audio in them. The
//   payload is whatever handle_endpoint_in() produced -- today, silence.
//   When real microphone samples arrive from the TV, they are filled in at
//   the handler and this pacing continues to apply unchanged.
//
// REQUIRED HEADERS (pulled in by main.cpp before this file):
//   <atomic> <chrono> <condition_variable> <cstdint> <deque> <functional>
//   <mutex> <thread> <vector>

// Bytes per second of the DualSense capture stream.
//   2 channels x 2 bytes (S16_LE) x 48000 Hz = 192000.
// MEASURED, not assumed: /proc/asound/card2/stream0 on an unrooted C1
// reports Capture interface 2 altset 1, S16_LE, 2 channels, 48000 Hz; and
// Windows reports the bridged endpoint as "2 channel, 16 bit, 48000 Hz".
// Both 2026-08-02.
// TODO: move to the device config file once capture is configurable.
static constexpr uint32_t kIsoInBytesPerSecond = 192000u;

// A completion held back until its audio duration has elapsed.
struct IsoInPending {
    uint32_t seqnum = 0;
    int32_t status = 0;
    uint32_t actualLength = 0;
    uint32_t startFrame = 0;
    std::vector<uint8_t> payload;
    uint32_t packets = 0;
    std::vector<uint8_t> isoDescriptors;
    uint32_t durationUs = 0;
};

class IsoInPacer {
public:
    // Mirrors CtmUsbipServer::send_submit. Returns false if the socket died.
    using SendFn = std::function<bool(uint32_t seqnum,
                                      int32_t status,
                                      uint32_t actualLength,
                                      uint32_t startFrame,
                                      const std::vector<uint8_t> &payload,
                                      uint32_t packets,
                                      const std::vector<uint8_t> &isoDescriptors)>;
    // Called once if a send fails, so the session can be torn down.
    using FailFn = std::function<void()>;

    IsoInPacer() = default;
    IsoInPacer(const IsoInPacer &) = delete;
    IsoInPacer &operator=(const IsoInPacer &) = delete;

    ~IsoInPacer() { stop(); }

    void start(SendFn send, FailFn onFail)
    {
        if (running_.exchange(true)) {
            return;
        }
        send_ = std::move(send);
        onFail_ = std::move(onFail);
        worker_ = std::thread([this]() { loop(); });
    }

    // How long `byteCount` of capture audio represents, in microseconds.
    // Clamped the same way the outbound path clamps its own duration.
    static uint32_t duration_us_for_bytes(uint32_t byteCount)
    {
        if (byteCount == 0) {
            return 0;
        }
        const uint64_t us = (static_cast<uint64_t>(byteCount) * 1000000ULL +
                             kIsoInBytesPerSecond - 1) /
                            kIsoInBytesPerSecond;
        if (us == 0) {
            return 1;
        }
        return static_cast<uint32_t>(us > 100000ULL ? 100000ULL : us);
    }

    // Queue a completion. Returns false if the pacer is not running, in
    // which case the caller must send it itself -- never drop a URB.
    bool submit(IsoInPending item)
    {
        if (!running_.load()) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= kMaxQueued) {
                // Host has more in flight than makes sense. Refuse rather
                // than grow without bound; the caller sends it immediately.
                ++refused_;
                return false;
            }
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
        return true;
    }

    void stop()
    {
        if (!running_.exchange(false)) {
            return;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    // Diagnostics, read by the usbip summary line.
    uint64_t paced() const { return paced_.load(std::memory_order_relaxed); }
    uint64_t refused() const { return refused_; }
    uint64_t wait_us_total() const { return waitUsTotal_.load(std::memory_order_relaxed); }
    size_t depth()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    static constexpr size_t kMaxQueued = 256;

    void loop()
    {
        using clock = std::chrono::steady_clock;
        auto nextDue = clock::time_point{};
        while (running_.load()) {
            IsoInPending item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return !queue_.empty() || !running_.load(); });
                if (!running_.load()) {
                    break;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
            }

            const auto paceStart = clock::now();
            if (item.durationUs != 0) {
                // Same catch-up guard as the outbound worker: if the
                // baseline is unset or has fallen far behind (a stall, a
                // reseat), reset it rather than firing a burst to "catch
                // up" -- a burst is exactly the busy loop being fixed.
                if (nextDue == clock::time_point{} ||
                    paceStart > nextDue + std::chrono::milliseconds(50)) {
                    nextDue = paceStart;
                }
                nextDue += std::chrono::microseconds(item.durationUs);
                if (nextDue > paceStart) {
                    std::this_thread::sleep_until(nextDue);
                }
            }
            const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                                    clock::now() - paceStart).count();
            waitUsTotal_.fetch_add(static_cast<uint64_t>(waited < 0 ? 0 : waited),
                                   std::memory_order_relaxed);
            paced_.fetch_add(1, std::memory_order_relaxed);

            if (!running_.load()) {
                break;
            }
            if (send_ && !send_(item.seqnum,
                                item.status,
                                item.actualLength,
                                item.startFrame,
                                item.payload,
                                item.packets,
                                item.isoDescriptors)) {
                if (onFail_) {
                    onFail_();
                }
                break;
            }
        }
    }

    std::atomic_bool running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<IsoInPending> queue_;
    SendFn send_;
    FailFn onFail_;
    std::atomic<uint64_t> paced_{0};
    std::atomic<uint64_t> waitUsTotal_{0};
    uint64_t refused_ = 0;
};
