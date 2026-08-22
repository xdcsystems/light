#pragma once

#include <cstddef>
#include <utility>

template <typename T>
class TransportWrapper {
public:
    TransportWrapper() = default;

    template <typename... Args>
    explicit TransportWrapper(Args&&... args)
        : _t(std::forward<Args>(args)...) {}

    bool init() {
        return _t.init();
    }

    bool send(const unsigned char* data, std::size_t len) {
        return _t.send(data, len);
    }

private:
    T _t;
};
