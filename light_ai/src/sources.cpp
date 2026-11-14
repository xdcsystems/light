#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sources.hpp"

template class BasicUsbNodeLocator<UsbHost>;
template class BasicSourceChannel<MediaSource, MppDecoder>;

namespace
{
    // A node that only supports read()/write() I/O passes the CAPTURE check
    // but dies at QUERYBUF with a confusing error: rejected here, where the
    // diagnosis is clear.
    bool usb_capture_node( const std::string& path )
    {
        const int fd = ::open( path.c_str(), O_RDONLY | O_CLOEXEC );
        if ( fd < 0 )
            return false;
        v4l2_capability cap {};
        while ( ::ioctl( fd, VIDIOC_QUERYCAP, &cap ) < 0 && errno == EINTR )
        {}
        bool ok = false;
        if ( ( cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ) && ( cap.capabilities & V4L2_CAP_STREAMING ) )
        {
            const char* bus = reinterpret_cast<const char*>( cap.bus_info );
            ok = std::strstr( bus, "usb" ) != nullptr;
        }
        ::close( fd );
        return ok;
    }

    // "videoN" with N all digits; fills n.
    bool numeric_video_name( const char* name, int& n )
    {
        if ( std::strncmp( name, "video", 5 ) != 0 || name[5] == '\0' )
            return false;
        for ( const char* p = name + 5; *p != '\0'; ++p )
        {
            if ( std::isdigit( static_cast<unsigned char>( *p ) ) == 0 )
                return false;
        }
        n = static_cast<int>( std::strtol( name + 5, nullptr, 10 ) );
        return true;
    }
}  // namespace

bool UsbHost::exists( const std::string& path ) const
{
    struct stat st {};
    return stat( path.c_str(), &st ) == 0;
}

int UsbHost::usb_capture_nodes( std::array<int, kMaxCandidates>& out ) const
{
    int nc = 0;
    DIR* d = opendir( "/dev" );
    if ( d == nullptr )
        return 0;
    const dirent* e = nullptr;
    while ( ( e = readdir( d ) ) != nullptr && nc < static_cast<int>( out.size() ) )
    {
        int n = 0;
        if ( !numeric_video_name( e->d_name, n ) )
            continue;
        const std::string p = std::string( "/dev/" ) + e->d_name;
        if ( exists( p ) && usb_capture_node( p ) )
            out[static_cast<size_t>( nc++ )] = n;
    }
    closedir( d );
    return nc;
}

long long UsbHost::now_ms() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}
