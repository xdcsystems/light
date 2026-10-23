#include "camera_v4l2.hpp"
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

CameraV4L2::CameraV4L2(const std::string& dev, int w, int h)
    : width_(w), height_(h) {
    fd_ = open(dev.c_str(), O_RDWR);
    if (fd_ < 0) { perror("open camera"); throw std::runtime_error("open failed"); }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    xioctl(VIDIOC_S_FMT, &fmt);

    v4l2_requestbuffers req{};
    req.count = buf_count_;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(VIDIOC_REQBUFS, &req);

    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        xioctl(VIDIOC_QUERYBUF, &buf);
        bufs_[i].length = buf.length;
        bufs_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd_, buf.m.offset);
        if (bufs_[i].start == MAP_FAILED)
            throw std::runtime_error("mmap failed");
        xioctl(VIDIOC_QBUF, &buf);
    }
}

CameraV4L2::~CameraV4L2() {
    if (fd_ >= 0) {
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &t);
        for (unsigned i = 0; i < buf_count_ && bufs_[i].start; ++i)
            munmap(bufs_[i].start, bufs_[i].length);
        close(fd_);
    }
}

void CameraV4L2::xioctl(unsigned long req, void* arg) const {
    int r;
    do { r = ioctl(fd_, req, arg); } while (r < 0 && errno == EINTR);
    if (r < 0) throw std::runtime_error(std::string("ioctl failed: ") + strerror(errno));
}

void CameraV4L2::start() {
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(VIDIOC_STREAMON, &t);
}

int CameraV4L2::dequeue(uint32_t* size) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    while (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return -1;
    }
    if (size) *size = buf.bytesused;
    return buf.index;
}

void CameraV4L2::requeue(int index) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    ioctl(fd_, VIDIOC_QBUF, &buf);
}
