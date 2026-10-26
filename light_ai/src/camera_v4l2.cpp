#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "camera_v4l2.hpp"
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <map>

CameraV4L2::CameraV4L2(int w, int h)
    : width_(w), height_(h) {}

// Full open+setup path for the node given by the owner: open + S_FMT +
// G_FMT verification + REQBUFS/mmap/QBUF. Nothing is trusted blindly: a
// substituted camera mode is caught here exactly like at startup.
// Non-throwing: failures are reported via the return value and logged
// here.
int CameraV4L2::setup_device(const std::string& dev, std::string* why) {
    fd_ = ::open(dev.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "open camera: " << strerror(errno) << '\n';
        if (why) *why = "open failed";
        return -1;
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl_r(VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "camera: S_FMT: " << strerror(errno) << '\n';
        if (why) *why = "S_FMT failed";
        return -1;
    }

    // The driver may silently substitute a closest supported mode.
    // Verify what we actually got and fail loudly on a mismatch,
    // otherwise the whole pipeline (RGA, MPP) would run with wrong dims.
    v4l2_format gfmt{};
    gfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl_r(VIDIOC_G_FMT, &gfmt) < 0) {
        std::cerr << "camera: G_FMT: " << strerror(errno) << '\n';
        if (why) *why = "G_FMT failed";
        return -1;
    }
    if (gfmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
        gfmt.fmt.pix.width != static_cast<unsigned>(width_) ||
        gfmt.fmt.pix.height != static_cast<unsigned>(height_)) {
        std::cerr << "camera: mode mismatch: requested "
                  << width_ << 'x' << height_ << " MJPEG, got "
                  << gfmt.fmt.pix.width << 'x' << gfmt.fmt.pix.height
                  << " fourcc=" << std::hex << gfmt.fmt.pix.pixelformat
                  << std::dec << '\n';
        if (why) *why = "camera mode mismatch";
        return -1;
    }

    v4l2_requestbuffers req{};
    req.count = kBufCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl_r(VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "camera: REQBUFS: " << strerror(errno) << '\n';
        if (why) *why = "REQBUFS failed";
        return -1;
    }
    if (req.count == 0) {
        if (why) *why = "camera: REQBUFS returned zero buffers";
        return -1;
    }

    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl_r(VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "camera: QUERYBUF idx=" << i << ": "
                      << strerror(errno) << '\n';
            if (why) *why = "QUERYBUF failed";
            return -1;
        }
        bufs_[i].length = buf.length;
        bufs_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd_, buf.m.offset);
        if (bufs_[i].start == MAP_FAILED) {
            bufs_[i].start = nullptr;
            std::cerr << "camera: mmap idx=" << i << ": "
                      << strerror(errno) << '\n';
            if (why) *why = "mmap failed";
            return -1;
        }
        if (xioctl_r(VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "camera: QBUF idx=" << i << ": "
                      << strerror(errno) << '\n';
            if (why) *why = "QBUF failed";
            return -1;
        }
    }
    return 0;
}

// Release the fd and all mappings. Safe on a partially-open device and
// on an already-dead one: STREAMOFF errors are ignored on purpose (the
// fd may not exist anymore), unmapped slots are skipped via the
// start==nullptr guard.
void CameraV4L2::teardown() {
    if (fd_ >= 0) {
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &t);
        close(fd_);
        fd_ = -1;
    }
    for (unsigned i = 0; i < kBufCount && bufs_[i].start; ++i) {
        munmap(bufs_[i].start, bufs_[i].length);
        bufs_[i].start = nullptr;
        bufs_[i].length = 0;
    }
}

// True when the node is a V4L2 capture device sitting on a USB bus
// (QUERYCAP bus_info contains "usb"). Everything else on this board —
// rkcif/rkisp platform nodes — is rejected before any S_FMT is tried:
// they would burn reconnect attempts on setup failures, and a platform
// capture node might even accept the format while never delivering a
// frame (no sensor attached) — a false "successful" reconnect.
namespace {
bool usb_capture_node(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    v4l2_capability cap{};
    while (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 && errno == EINTR) {}
    bool ok = false;
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        const char* bus = reinterpret_cast<const char*>(cap.bus_info);
        ok = bus != nullptr && std::strstr(bus, "usb") != nullptr;
    }
    ::close(fd);
    return ok;
}
}  // anonymous namespace (file-local helper)

// Disconnect recovery: full teardown + the same setup path as startup,
// plus STREAMON. The node is supplied by the owner (see
// find_usb_capture_node) — the class never decides which node to open.
int CameraV4L2::reconnect(const std::string& dev, std::string* why) {
    teardown();
    if (setup_device(dev, why) != 0) {
        teardown();
        return -1;
    }
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl_r(VIDIOC_STREAMON, &t) < 0) {
        std::cerr << "camera: reconnect STREAMON: " << strerror(errno) << '\n';
        teardown();
        return -1;
    }
    return 0;
}

// Node discovery. Preferred: the configured path, if it exists and its
// full setup passes; otherwise the numeric /dev/video* candidates are
// filtered by the USB capture check and the first one whose setup passes
// wins. The trial leaves the camera closed on every outcome — the
// caller re-opens the winning node via reconnect().
int find_usb_capture_node(CameraV4L2& cam, const std::string& preferred,
                          std::string& out) {
    struct stat st{};
    std::string why;
    if (!preferred.empty() && stat(preferred.c_str(), &st) == 0) {
        if (cam.setup_device(preferred, &why) == 0) {
            cam.teardown();
            out = preferred;
            return 0;
        }
        std::cerr << "camera: reconnect setup on " << preferred
                  << " failed: " << why << '\n';
        cam.teardown();
    }
    std::cerr << "camera: node " << preferred
              << " not present, scanning /dev/video*\n";
    std::map<int, std::string> cands;
    DIR* d = opendir("/dev");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            const std::string n = e->d_name;
            if (n.rfind("video", 0) != 0 || n.size() == 5) continue;
            bool digits = true;
            for (size_t i = 5; i < n.size() && digits; ++i)
                digits = isdigit(static_cast<unsigned char>(n[i])) != 0;
            if (!digits) continue;
            const std::string p = "/dev/" + n;
            struct stat cst{};
            if (stat(p.c_str(), &cst) == 0 && usb_capture_node(p))
                cands[std::atoi(n.c_str() + 5)] = p;
        }
        closedir(d);
    }
    for (const auto& kv : cands) {
        std::cerr << "camera: trying " << kv.second << '\n';
        if (cam.setup_device(kv.second, &why) == 0) {
            cam.teardown();
            out = kv.second;
            return 0;
        }
        cam.teardown();
    }
    return -1;
}

CameraV4L2::~CameraV4L2() {
    teardown();
}

void CameraV4L2::xioctl(unsigned long req, void* arg) const {
    int r;
    do { r = ioctl(fd_, req, arg); } while (r < 0 && errno == EINTR);
    if (r < 0) {
        throw std::runtime_error(std::string("ioctl failed: ") + strerror(errno));
    }
}

int CameraV4L2::xioctl_r(unsigned long req, void* arg) const {
    int r;
    do { r = ioctl(fd_, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

void CameraV4L2::start() {
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(VIDIOC_STREAMON, &t);
}

int CameraV4L2::dequeue(uint32_t* size, int* err) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    while (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        if (err) *err = errno;
        return -1;
    }
    if (err) *err = 0;
    if (size) *size = buf.bytesused;
    return buf.index;
}

void CameraV4L2::requeue(int index) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        // A failed QBUF silently removes the buffer from rotation;
        // without this log the symptom is only a mysterious cam-time growth.
        std::cerr << "camera: QBUF failed for idx=" << index << ": "
                  << strerror(errno) << '\n';
    }
}
