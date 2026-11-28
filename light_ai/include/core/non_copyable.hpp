// non_copyable.hpp: the only base classes the project allows (CONTRIBUTING, No virtuals or inheritance).
// Usage:
//   class X : private NonMovable<X>    - X can be neither copied nor moved;
//   class Y : private NonCopyable<Y>   - Y cannot be copied; a move member that Y declares itself stays as written.
// Both are empty and non-virtual: no vtable, and the empty base takes no space in the derived class.
// Tag: the derived class, so each class has its own base type and a first member with such a base of its own
// does not cost the outer class its empty base.

#pragma once

template <class Tag>
class NonCopyable
{
public:
    NonCopyable( const NonCopyable& ) = delete;
    NonCopyable& operator=( const NonCopyable& ) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable( NonCopyable&& ) = default;
    NonCopyable& operator=( NonCopyable&& ) = default;
};

template <class Tag>
class NonMovable
{
public:
    NonMovable( const NonMovable& ) = delete;
    NonMovable& operator=( const NonMovable& ) = delete;
    NonMovable( NonMovable&& ) = delete;
    NonMovable& operator=( NonMovable&& ) = delete;

protected:
    NonMovable() = default;
    ~NonMovable() = default;
};
