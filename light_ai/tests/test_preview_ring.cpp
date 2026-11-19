#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/preview_ring.hpp"
#include "suites.hpp"

namespace
{
    constexpr size_t kFrameBytes = 64 * 1024;

    bool is_uniform( const std::vector<uint8_t>& buf, uint8_t v )
    {
        return buf.size() == kFrameBytes && std::all_of( buf.begin(), buf.end(), [v]( uint8_t b ) { return b == v; } );
    }
}  // namespace

void test_preview_ring( TestRun& t )
{
    // Closed ring ignores push; open/close isolates sessions.
    {
        PreviewRing ring( kFrameBytes );
        const std::vector<uint8_t> f1( kFrameBytes, 1 );
        const std::vector<uint8_t> f2( kFrameBytes, 2 );
        const std::vector<uint8_t> f3( kFrameBytes, 3 );
        std::vector<uint8_t> out( kFrameBytes, 0 );

        CHECK( t, !ring.push( f1.data() ) );
        ring.open();
        CHECK( t, ring.push( f1.data() ) );
        CHECK( t, ring.push( f2.data() ) );
        // 3rd push drops oldest (f1) and keeps f2, f3.
        CHECK( t, ring.push( f3.data() ) );
        CHECK( t, ring.drops() == 1 );

        CHECK( t, ring.pop( out ) && is_uniform( out, 2 ) );
        CHECK( t, ring.pop( out ) && is_uniform( out, 3 ) );

        // Closing discards unread frames and wakes waiting consumer.
        CHECK( t, ring.push( f1.data() ) );
        ring.close();
        CHECK( t, !ring.push( f2.data() ) );
        CHECK( t, !ring.pop( out ) );
    }

    // stop() unblocks a waiting pop().
    {
        PreviewRing ring( kFrameBytes );
        ring.open();
        std::atomic<bool> popped { true };
        std::thread consumer( [&] {
            std::vector<uint8_t> out( kFrameBytes, 0 );
            popped.store( ring.pop( out ) );
        } );
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
        ring.stop();
        consumer.join();
        CHECK( t, !popped.load() );
    }

    // push_rows (M-11): a padded producer (stride > row) is packed into the
    // slot -- the padding never reaches the consumer; bad geometry is refused.
    {
        constexpr size_t row = 12;
        constexpr size_t rows = 3;
        constexpr size_t stride = 16;
        PreviewRing ring( row * rows );
        std::vector<uint8_t> src( stride * rows, 0xEE );
        for ( size_t r = 0; r < rows; ++r )
            for ( size_t i = 0; i < row; ++i )
                src[r * stride + i] = static_cast<uint8_t>( r * row + i );
        std::vector<uint8_t> out( row * rows, 0 );
        ring.open();
        CHECK( t, !ring.push_rows( src.data(), row, row - 1 ) );  // stride shorter than a row
        CHECK( t, !ring.push_rows( src.data(), 5, stride ) );      // row does not divide the frame
        CHECK( t, !ring.push_rows( src.data(), 0, stride ) );
        CHECK( t, ring.push_rows( src.data(), row, stride ) );
        CHECK( t, ring.pop( out ) );
        bool packed = true;
        for ( size_t i = 0; i < out.size(); ++i )
            packed = packed && out[i] == static_cast<uint8_t>( i );
        CHECK( t, packed );
        ring.stop();
    }
}

void test_preview_ring_threads( TestRun& t )
{
    PreviewRing ring( kFrameBytes );
    ring.open();
    std::atomic<bool> done { false };
    std::atomic<bool> torn { false };
    std::atomic<int> consumed { 0 };

    // Slow consumer simulates av_write_frame() holding send_buf_ unlocked across iterations.
    std::thread consumer( [&] {
        std::vector<uint8_t> send_buf( kFrameBytes, 0 );
        while ( ring.pop( send_buf ) )
        {
            const uint8_t tag = send_buf[0];
            if ( !is_uniform( send_buf, tag ) )
                torn.store( true );
            consumed.fetch_add( 1 );
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    } );

    std::thread producer( [&] {
        std::vector<uint8_t> frame( kFrameBytes, 0 );
        for ( int i = 1; i <= 120; ++i )
        {
            const uint8_t tag = static_cast<uint8_t>( i & 0xFF );
            std::fill( frame.begin(), frame.end(), tag );
            static_cast<void>( ring.push( frame.data() ) );
            std::this_thread::sleep_for( std::chrono::microseconds( 250 ) );
        }
        done.store( true );
        ring.close();
    } );

    producer.join();
    consumer.join();
    CHECK( t, done.load() && !torn.load() && consumed.load() > 0 && ring.drops() > 0 );
}
