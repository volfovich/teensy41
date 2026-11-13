#pragma once
// Minimal random-access byte stream, so file readers in lib/dsp stay portable:
// the firmware wraps an SD file, host tests wrap a FILE* or a memory buffer.

#include <cstdint>
#include <cstring>

namespace dsp {

class ByteSource {
public:
    virtual ~ByteSource() {}
    virtual int read(void* dst, int n) = 0;   // returns bytes read
    virtual bool seek(uint32_t pos) = 0;
};

// A ByteSource over a block of memory (flash-embedded files, tests).
class MemorySource : public ByteSource {
public:
    MemorySource(const void* data, uint32_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}
    int read(void* dst, int n) override {
        if (n < 0) return 0;
        uint32_t count = static_cast<uint32_t>(n);
        if (count > size_ - pos_) count = size_ - pos_;
        std::memcpy(dst, data_ + pos_, count);
        pos_ += count;
        return static_cast<int>(count);
    }
    bool seek(uint32_t pos) override {
        if (pos > size_) return false;
        pos_ = pos;
        return true;
    }

private:
    const uint8_t* data_;
    uint32_t size_;
    uint32_t pos_ = 0;
};

} // namespace dsp
