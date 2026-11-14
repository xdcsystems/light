#include <cstddef>
#include <cstdint>
#include <cstring>

#include "check.hpp"
#include "core/byte_window.hpp"
#include "suites.hpp"

void test_byte_window( TestRun& t )
{
    ByteWindow<8> w;
    CHECK( t, w.size() == 0 && w.room() == 8 && !w.full() && w.capacity() == 8 );
    const uint8_t abc[] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i' };
    CHECK( t, w.append( abc, 5 ) == 5 );
    CHECK( t, w.size() == 5 && std::memcmp( w.data(), "abcde", 5 ) == 0 );
    w.consume( 2 );
    CHECK( t, w.size() == 3 && w.data()[0] == 'c' );
    // room() counts from the write position; tail() compacts first.
    CHECK( t, w.room() == 3 );
    uint8_t* tl = w.tail();
    CHECK( t, w.room() == 5 && w.data()[0] == 'c' && tl == w.data() + 3 );
    std::memcpy( tl, "XYZ", 3 );
    w.commit( 3 );
    CHECK( t, w.size() == 6 && std::memcmp( w.data(), "cdeXYZ", 6 ) == 0 );
    // append() takes what fits; a full window has no room.
    CHECK( t, w.append( abc, 9 ) == 2 );
    CHECK( t, w.full() && w.room() == 0 );
    w.tail();
    CHECK( t, w.room() == 0 );
    // consume() clamps; clear() drops everything.
    w.consume( 100 );
    CHECK( t, w.size() == 0 && !w.full() );
    w.append( abc, 4 );
    w.clear();
    CHECK( t, w.size() == 0 && w.room() == 8 );
    // commit() clamps to the room.
    w.tail();
    w.commit( 100 );
    CHECK( t, w.size() == 8 && w.full() );
}
