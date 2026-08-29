// 単一生産者・単一消費者(SPSC)のロックフリー・リングバッファ。
// オーディオのキャプチャ側スレッドとレンダ側スレッドを橋渡しする。
#pragma once
#include <atomic>
#include <vector>
#include <cstddef>
#include <algorithm>

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : buf_(capacity + 1), cap_(capacity + 1) {}

    // 書き込み可能な要素数
    size_t writeAvailable() const {
        size_t w = wr_.load(std::memory_order_relaxed);
        size_t r = rd_.load(std::memory_order_acquire);
        return (r + cap_ - w - 1) % cap_;
    }
    // 読み出し可能な要素数
    size_t readAvailable() const {
        size_t w = wr_.load(std::memory_order_acquire);
        size_t r = rd_.load(std::memory_order_relaxed);
        return (w + cap_ - r) % cap_;
    }

    // 最大 n 要素を書き込む。実際に書けた数を返す。
    size_t push(const float* src, size_t n) {
        size_t w = wr_.load(std::memory_order_relaxed);
        size_t r = rd_.load(std::memory_order_acquire);
        size_t space = (r + cap_ - w - 1) % cap_;
        n = std::min(n, space);
        for (size_t i = 0; i < n; i++) {
            buf_[w] = src[i];
            w = (w + 1) % cap_;
        }
        wr_.store(w, std::memory_order_release);
        return n;
    }

    // 最大 n 要素を読み出す。実際に読めた数を返す。
    size_t pop(float* dst, size_t n) {
        size_t w = wr_.load(std::memory_order_acquire);
        size_t r = rd_.load(std::memory_order_relaxed);
        size_t avail = (w + cap_ - r) % cap_;
        n = std::min(n, avail);
        for (size_t i = 0; i < n; i++) {
            dst[i] = buf_[r];
            r = (r + 1) % cap_;
        }
        rd_.store(r, std::memory_order_release);
        return n;
    }

    // 読み側から古いデータを n 要素捨てる（過充填時のドリフト補正）。
    size_t drop(size_t n) {
        size_t w = wr_.load(std::memory_order_acquire);
        size_t r = rd_.load(std::memory_order_relaxed);
        size_t avail = (w + cap_ - r) % cap_;
        n = std::min(n, avail);
        rd_.store((r + n) % cap_, std::memory_order_release);
        return n;
    }

    size_t capacity() const { return cap_ - 1; }

private:
    std::vector<float> buf_;
    size_t cap_;
    std::atomic<size_t> wr_{0};
    std::atomic<size_t> rd_{0};
};
