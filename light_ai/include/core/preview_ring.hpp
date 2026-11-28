// BasicPreviewRing: pre-allocated 2-slot drop-oldest ring (single producer, single consumer) for decoupling
// NetPreview::publish() (output thread, StreamOutput) from av_write_frame()/avio_flush()
// (preview serve thread).
//
// Thread rule (every member is guarded by mx_, except write_buf_: the one
// producer thread owns it and fills it unlocked):
//   output thread (producer, StreamOutput): push(), push_rows(), drops();
//   serve thread (consumer):    open(), close(), pop();
//   the owner's destructor:     stop() (wakes a blocked pop for good).
//
// Startup-only allocation: both ring slots and the producer's write buffer are
// sized once to frame_bytes in the constructor, and the consumer owns a
// pre-allocated send_buf_ of the same size. Buffers change hands by
// std::vector::swap() in O(1) under mx_. Zero per-frame heap allocations, and
// neither the frame copy nor socket I/O happens under the lock.
//
// Production instantiation: BasicPreviewRing<PiMutex, PiCondVar> in
// net/net_preview.hpp; host tests use PreviewRing = BasicPreviewRing<>.

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/non_copyable.hpp"

template <class Mutex = std::mutex, class CondVar = std::condition_variable>
class BasicPreviewRing : private NonMovable<BasicPreviewRing<Mutex, CondVar>>
{
public:
    explicit BasicPreviewRing( size_t frame_bytes ) : frame_bytes_( frame_bytes ), write_buf_( frame_bytes )
    {
        if ( frame_bytes_ > 0 )
        {
            slots_[0].resize( frame_bytes_ );
            slots_[1].resize( frame_bytes_ );
        }
    }

    void open() noexcept
    {
        {
            const std::lock_guard<Mutex> lk( mx_ );
            head_ = 0;
            count_ = 0;
            open_ = true;
        }
        cv_.notify_all();
    }

    void close() noexcept
    {
        {
            const std::lock_guard<Mutex> lk( mx_ );
            open_ = false;
            head_ = 0;
            count_ = 0;
        }
        cv_.notify_all();
    }

    void stop() noexcept
    {
        {
            const std::lock_guard<Mutex> lk( mx_ );
            stopped_ = true;
            open_ = false;
            head_ = 0;
            count_ = 0;
        }
        cv_.notify_all();
    }

    // Producer (one thread): copies frame_bytes_ into the ring, dropping
    // the oldest unread slot if full. Returns false when closed/stopped/empty.
    bool push( const uint8_t* data ) noexcept
    {
        return push_rows( data, frame_bytes_, frame_bytes_ );
    }

    // push() for a padded source: frame_bytes_ / row_bytes rows of row_bytes,
    // read every src_stride bytes, packed into the slot. src_stride ==
    // row_bytes is the plain copy; row_bytes must divide frame_bytes_ and
    // src_stride must be >= row_bytes, else false.
    bool push_rows( const uint8_t* data, size_t row_bytes, size_t src_stride ) noexcept
    {
        if ( data == nullptr || frame_bytes_ == 0 || row_bytes == 0 || frame_bytes_ % row_bytes != 0 || src_stride < row_bytes )
            return false;
        if ( !accepting() )
            return false;  // no viewer: nothing is copied
        if ( src_stride == row_bytes )
            std::memcpy( write_buf_.data(), data, frame_bytes_ );
        else
            for ( size_t off = 0, src = 0; off < frame_bytes_; off += row_bytes, src += src_stride )
                std::memcpy( write_buf_.data() + off, data + src, row_bytes );
        {
            const std::lock_guard<Mutex> lk( mx_ );
            if ( !open_ || stopped_ )
                return false;  // closed or stopped during the copy
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
            slots_[idx].swap( write_buf_ );  // both hold at least frame_bytes_
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
        std::unique_lock<Mutex> lk( mx_ );
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
        const std::lock_guard<Mutex> lk( mx_ );
        return drops_;
    }

private:
    bool accepting() const noexcept
    {
        const std::lock_guard<Mutex> lk( mx_ );
        return open_ && !stopped_;
    }

private:
    const size_t frame_bytes_;
    mutable Mutex mx_;
    CondVar cv_;
    std::array<std::vector<uint8_t>, 2> slots_ {};
    std::vector<uint8_t> write_buf_;  // producer only, outside mx_; sized in the constructor, never resized
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t drops_ = 0;
    bool open_ = false;
    bool stopped_ = false;
};

using PreviewRing = BasicPreviewRing<>;
