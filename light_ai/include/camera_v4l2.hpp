#pragma once
#include <string>
#include <cstdint>
#include <linux/videodev2.h>

class CameraV4L2 {
public:
    CameraV4L2(const std::string& dev, int width, int height);
    ~CameraV4L2();

    void start();
    // Returns the buffer index; size is filled with the actual frame length
    int dequeue(uint32_t* size);
    void requeue(int index);
    void* buffer(int index) const { return bufs_[index].start; }
    size_t buffer_size(int index) const { return bufs_[index].length; }
    int width()  const { return width_; }
    int height() const { return height_; }

private:
    int fd_ = -1;
    int width_ = 0, height_ = 0;
    unsigned buf_count_ = 4;
    struct Buffer { void* start = nullptr; size_t length = 0; };
    Buffer bufs_[4];
    void xioctl(unsigned long req, void* arg) const;
};
