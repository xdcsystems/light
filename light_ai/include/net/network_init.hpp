// NetworkInit: avformat_network_init() for the lifetime of the holder,
// avformat_network_deinit() in its destructor. NetEncoder declares it as
// its first member: the network layer is up before any other member is
// built and goes down after all of them (the serve thread included).

#pragma once

class NetworkInit
{
public:
    NetworkInit();
    ~NetworkInit();
    NetworkInit( const NetworkInit& ) = delete;
    NetworkInit& operator=( const NetworkInit& ) = delete;
};
