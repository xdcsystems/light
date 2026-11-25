// Fakes for the source-layer templates (source/): a device, a decoder and a
// host, each driven by a control block the test owns. No SDK, no heap.

#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "source/usb_node_locator.hpp"
#include "source/source_types.hpp"

// Device control: what the next calls return, and what was called.
// Attach-path fields and frame-path fields are separate memory: under the
// SourceChannel thread rule the two threads never touch the same ones.
struct FakeDevice
{
    // attach path
    int open_rc = 0;
    int trial_rc = 0;
    int w = 1280;
    int h = 960;
    int opens = 0;
    int teardowns = 0;
    bool net = false;
    std::vector<std::string> trials;
    std::string opened;
    // frame path
    int dq_buf = 0;   // >= 0: a frame in this buffer; < 0: fail with dq_err
    int dq_err = EAGAIN;
    uint32_t dq_size = 100;
    int dequeues = 0;
    int requeues = 0;
    int poll_fd = -1;
};

class FakeSrc
{
public:
    FakeSrc( const Log&, FakeDevice& d ) : d_( d ) {}

    void use_camera() { d_.net = false; }
    void use_net() { d_.net = true; }
    void set_cancel( const std::atomic<bool>*, const std::atomic<bool>* ) noexcept {}
    void set_wake_fd( int ) noexcept {}
    int poll_fd() const noexcept { return d_.poll_fd; }
    Status open( const std::string& id )
    {
        ++d_.opens;
        d_.opened = id;
        if ( d_.open_rc != 0 )
            return make_status( Errc::Absent, "open refused" );
        return {};
    }
    Status trial( const std::string& node )
    {
        d_.trials.push_back( node );
        if ( d_.trial_rc != 0 )
            return make_status( Errc::Failed, "trial refused" );
        return {};
    }
    Status dequeue( int& index, uint32_t& size, int )
    {
        ++d_.dequeues;
        if ( d_.dq_buf < 0 )
            return dequeue_status( d_.dq_err );
        index = d_.dq_buf;
        size = d_.dq_size;
        return {};
    }
    void requeue( int ) { ++d_.requeues; }
    void* buffer( int ) const { return bytes_.data(); }
    int width() const { return d_.w; }
    int height() const { return d_.h; }
    int64_t last_cap_us() const noexcept { return -1; }
    uint64_t skip_parts_cnt() const noexcept { return 0; }
    void teardown() { ++d_.teardowns; }

private:
    FakeDevice& d_;
    mutable std::array<uint8_t, 16> bytes_ {};
};

struct FakeDecoderCtl
{
    int init_rc = 0;
    int decode_rc = 0;  // 0: Ok, 1: Timeout, 2: NoMemory
    int decode_fd = 7;  // < 0: decode error
    int aux_rc = 9;     // aux fd, -1 = alloc failure
    int hor_stride = 0;
    int ver_stride = 0;
    size_t last_aux_bytes = 0;
    int inits = 0;
    int decodes = 0;
    int aux_calls = 0;
};

class FakeDec
{
public:
    explicit FakeDec( FakeDecoderCtl& c ) : c_( c ) {}
    Status init( int, int )
    {
        ++c_.inits;
        return c_.init_rc == 0 ? Status{} : make_status( Errc::Failed, "fake init failure" );
    }
    Status decode( const uint8_t*, size_t, int& fd )
    {
        ++c_.decodes;
        if ( c_.decode_rc == 1 )
            return make_status( Errc::Timeout, "fake decode timeout" );
        if ( c_.decode_rc == 2 )
            return make_status( Errc::NoMemory, "fake decode oom" );
        fd = c_.decode_fd;
        return {};
    }
    int aux_fd( size_t bytes )
    {
        ++c_.aux_calls;
        c_.last_aux_bytes = bytes;
        return c_.aux_rc;
    }
    int out_fmt() const { return 0; }
    int hor_stride() const noexcept { return c_.hor_stride; }
    int ver_stride() const noexcept { return c_.ver_stride; }

private:
    FakeDecoderCtl& c_;
};

// /dev as a list: present nodes, USB capture node numbers, a settable clock.
struct FakeHost
{
    bool exists( const std::string& path ) const
    {
        for ( const std::string& p : present )
        {
            if ( p == path )
                return true;
        }
        return false;
    }
    int usb_capture_nodes( std::array<int, kMaxCandidates>& out ) const
    {
        int n = 0;
        for ( const int v : usb )
            out[static_cast<size_t>( n++ )] = v;
        return n;
    }
    long long now_ms() const { return now; }

public:
    std::vector<std::string> present;
    std::vector<int> usb;
    long long now = 1000;
};
