// tidy_probe/lookalike.cpp: custom-no-inheritance against look-alikes of the allowed bases (self-test fixture, see
// violations.cpp). Its own file: it must not include core/non_copyable.hpp, or the names would be ambiguous.

namespace
{
    template <class T>
    class NonMovable                                      // custom-no-inheritance (a look-alike definition)
    {
    };
}  // namespace
class AnonUser : private NonMovable<AnonUser>             // custom-no-inheritance (the anonymous-namespace one)
{
};

namespace other
{
    template <class T>
    class NonCopyable
    {
    };
}  // namespace other
class OtherUser : private other::NonCopyable<OtherUser>   // custom-no-inheritance (another namespace)
{
};
