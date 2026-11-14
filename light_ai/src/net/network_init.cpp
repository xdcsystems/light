extern "C"
{
#include <libavformat/avformat.h>
}

#include "net/network_init.hpp"

NetworkInit::NetworkInit()
{
    avformat_network_init();
}

NetworkInit::~NetworkInit()
{
    avformat_network_deinit();
}
