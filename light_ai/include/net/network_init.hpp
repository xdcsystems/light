// NetworkInit: avformat_network_init() for the lifetime of the holder,
// avformat_network_deinit() in its destructor. OverlaySink holds the one
// instance and declares it before preview_ and enc_: the network layer is up
// before either is built and goes down after both (their threads included).
// NetEncoder and NetPreview take a const reference to it as proof of that.

#pragma once

#include "core/non_copyable.hpp"

class NetworkInit : private NonMovable<NetworkInit>
{
public:
    NetworkInit();
    ~NetworkInit();
};
