// PreviewRing: pre-allocated 2-slot drop-oldest ring for decoupling
// NetPreview::publish() (pipeline thread) from av_write_frame()/avio_flush()
// (preview serve thread).
//
// Startup-only allocation: both ring slots are sized once to frame_bytes in
// the constructor, and the consumer owns a pre-allocated send_buf_ of the same
// size swapped via std::vector::swap() in O(1) under mx_. Zero per-frame heap
// allocations and zero socket-I/O lock hold on the pipeline thread.

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

class PreviewRing
{
public:
    explicit PreviewRing( size_t frame_bytes ) : frame_bytes_( frame_bytes )
    {
        if ( frame_bytes_ > 0 )
        {
            slots_[0].resize( frame_bytes_ );
            slots_[1].resize( frame_bytes_ );
        }
    }

    PreviewRing( const PreviewRing& ) = delete;
    PreviewRing& operator=( const PreviewRing& ) = delete;

    void open() noexcept
    {
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            head_ = 0;
            count_ = 0;
            open_ = true;
        }
        cv_.notify_all();
    }

    void close() noexcept
    {
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            open_ = false;
            head_ = 0;
            count_ = 0;
        }
        cv_.notify_all();
    }

    void stop() noexcept
    {
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            stopped_ = true;
            open_ = false;
            head_ = 0;
            count_ = 0;
        }
        cv_.notify_all();
    }

    // Producer (pipeline thread): copies frame_bytes_ into the ring, dropping
    // the oldest unread slot if full. Returns false when closed/stopped/empty.
    bool push( const uint8_t* data ) noexcept
    {
        if ( data == nullptr || frame_bytes_ == 0 )
            return false;
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            if ( !open_ || stopped_ )
                return false;
            size_t idx = 0;
            if ( count_ == 2 )
            {
                idx = head_;
                head_ = ( head_ + 1 ) & 1;
                ++drops_;
            }
            else
            {
                idx = ( head_ + count_ ) & 1;
                ++count_;
            }
            std::memcpy( slots_[idx].data(), data, frame_bytes_ );
        }
        cv_.notify_one();
        return true;
    }

    // Consumer (serve thread): blocks until a frame arrives or session closes /
    // ring stops. Precondition: out.size() >= frame_bytes_ (no heap allocation).
    bool pop( std::vector<uint8_t>& out ) noexcept
    {
        if ( frame_bytes_ == 0 || out.size() < frame_bytes_ )
            return false;
        std::unique_lock<std::mutex> lk( mx_ );
        cv_.wait( lk, [this] { return !open_ || stopped_ || count_ > 0; } );
        if ( !open_ || stopped_ || count_ == 0 )
            return false;
        out.swap( slots_[head_] );
        head_ = ( head_ + 1 ) & 1;
        --count_;
        return true;
    }

    size_t frame_bytes() const noexcept { return frame_bytes_; }

    uint64_t drops() const noexcept
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        return drops_;
    }

private:
    const size_t frame_bytes_;
    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::array<std::vector<uint8_t>, 2> slots_ {};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t drops_ = 0;
    bool open_ = false;
    bool stopped_ = false;
};
