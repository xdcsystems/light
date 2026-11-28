// NonCopyable / NonMovable (core/non_copyable.hpp): what a derived class can do, and that the base costs nothing
// (no vtable, no bytes).

#include <type_traits>
#include <utility>

#include "check.hpp"
#include "core/non_copyable.hpp"
#include "suites.hpp"

namespace
{
    class Pinned : private NonMovable<Pinned>
    {
    };

    class MoveOnly : private NonCopyable<MoveOnly>
    {
    public:
        explicit MoveOnly( int v ) noexcept : value( v ) {}

        int value;
    };

    struct WithInt : private NonMovable<WithInt>
    {
        int x = 0;
    };

    struct MoveOnlyWithInt : private NonCopyable<MoveOnlyWithInt>
    {
        int x = 0;
    };

    // the first member has an empty base too: a different base type, so both can sit at offset 0
    class Inner : private NonMovable<Inner>
    {
    public:
        int x = 0;
    };

    class Outer : private NonMovable<Outer>
    {
    public:
        Inner first;
    };

    // A user-declared destructor suppresses the implicit move of a derived class: NonCopyable alone does not keep it
    // movable.
    class WithDtor : private NonCopyable<WithDtor>
    {
    public:
        explicit WithDtor( int& count ) noexcept : count_( count ) {}
        ~WithDtor() { ++count_; }

    private:
        int& count_;
    };

    // The same class with its own move constructor: movable, still not copyable.
    class WithDtorAndMove : private NonCopyable<WithDtorAndMove>
    {
    public:
        explicit WithDtorAndMove( int& count ) noexcept : count_( &count ) {}
        WithDtorAndMove( WithDtorAndMove&& other ) noexcept : count_( other.count_ ) { other.count_ = nullptr; }
        ~WithDtorAndMove()
        {
            if ( count_ != nullptr )
                ++*count_;
        }

    private:
        int* count_;
    };

    // The bases add no destructor work of their own.
    static_assert( std::is_trivially_destructible_v<Pinned> );
    static_assert( std::is_trivially_destructible_v<MoveOnly> );
    static_assert( std::is_trivially_destructible_v<WithInt> );
    static_assert( std::is_trivially_destructible_v<MoveOnlyWithInt> );
    static_assert( std::is_trivially_destructible_v<Outer> );

    static_assert( !std::is_trivially_destructible_v<WithDtor> );
    static_assert( !std::is_copy_constructible_v<WithDtor> );
    static_assert( !std::is_move_constructible_v<WithDtor> );
    static_assert( std::is_nothrow_move_constructible_v<WithDtorAndMove> );
    static_assert( !std::is_copy_constructible_v<WithDtorAndMove> );

    static_assert( std::is_default_constructible_v<Pinned> );
    static_assert( std::is_destructible_v<Pinned> );
    static_assert( !std::is_copy_constructible_v<Pinned> );
    static_assert( !std::is_copy_assignable_v<Pinned> );
    static_assert( !std::is_move_constructible_v<Pinned> );
    static_assert( !std::is_move_assignable_v<Pinned> );
    static_assert( !std::is_polymorphic_v<Pinned> );

    static_assert( !std::is_copy_constructible_v<MoveOnly> );
    static_assert( !std::is_copy_assignable_v<MoveOnly> );
    static_assert( std::is_nothrow_move_constructible_v<MoveOnly> );
    static_assert( std::is_nothrow_move_assignable_v<MoveOnly> );
    static_assert( !std::is_polymorphic_v<MoveOnly> );

    // the bases are for deriving only, and a private base gives no conversion to it
    static_assert( !std::is_default_constructible_v<NonMovable<Pinned>> );
    static_assert( !std::is_destructible_v<NonMovable<Pinned>> );
    static_assert( !std::is_default_constructible_v<NonCopyable<MoveOnly>> );
    static_assert( !std::is_destructible_v<NonCopyable<MoveOnly>> );
    static_assert( !std::is_convertible_v<Pinned*, NonMovable<Pinned>*> );
    static_assert( !std::is_convertible_v<MoveOnly*, NonCopyable<MoveOnly>*> );

    static_assert( sizeof( WithInt ) == sizeof( int ) );
    static_assert( sizeof( MoveOnlyWithInt ) == sizeof( int ) );
    static_assert( sizeof( Outer ) == sizeof( Inner ) );
    static_assert( sizeof( Inner ) == sizeof( int ) );
}  // namespace

void test_non_copyable( TestRun& t )
{
    MoveOnly a { 7 };
    MoveOnly b { std::move( a ) };
    CHECK_EQ( t, b.value, 7 );

    MoveOnly c { 1 };
    c = std::move( b );
    CHECK_EQ( t, c.value, 7 );

    int destroyed = 0;
    {
        const WithDtor plain( destroyed );
        WithDtorAndMove from( destroyed );
        const WithDtorAndMove to( std::move( from ) );
    }
    CHECK_EQ( t, destroyed, 2 );  // plain and the move target; the moved-from object counts nothing
}
