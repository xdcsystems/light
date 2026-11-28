// NetworkInit: avformat_network_init() for the lifetime of the holder,
// avformat_network_deinit() in its destructor. NetEncoder declares it right
// after cfg_ (plain settings): the network layer is up before every later
// member is built and goes down after all of them (the serve thread included).
// NetPreview declares it before its packet and thread: both die before it.

#pragma once

class NetworkInit
{
public:
    NetworkInit();
    ~NetworkInit();
    NetworkInit( const NetworkInit& ) = delete;
    NetworkInit& operator=( const NetworkInit& ) = delete;
};
