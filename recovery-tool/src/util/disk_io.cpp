/*
 * disk_io.cpp - mmap-based disk image access with RAII
 */
#include "sdrecov.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

MappedFile::~MappedFile()
{
    if (data_)
        munmap(const_cast<uint8_t *>(data_), size_);
}

MappedFile::MappedFile(MappedFile &&other) noexcept
    : data_(other.data_), size_(other.size_)
{
    other.data_ = nullptr;
    other.size_ = 0;
}

MappedFile &MappedFile::operator=(MappedFile &&other) noexcept
{
    if (this != &other) {
        if (data_) munmap(const_cast<uint8_t *>(data_), size_);
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

bool MappedFile::open(const char *path)
{
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        log_error("cannot open %s", path);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("cannot stat %s", path);
        close(fd);
        return false;
    }

    void *ptr = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        log_error("cannot mmap %s (%zu bytes)", path, (size_t)st.st_size);
        return false;
    }

    madvise(ptr, st.st_size, MADV_SEQUENTIAL);

    data_ = static_cast<const uint8_t *>(ptr);
    size_ = st.st_size;
    return true;
}

const uint8_t *DiskImage::cluster_ptr(uint32_t cluster) const
{
    if (cluster < 2 || cluster >= geo.total_clusters + 2)
        return nullptr;

    uint64_t offset = geo.data_offset
                    + (uint64_t)(cluster - 2) * geo.bytes_per_cluster;

    if (offset + geo.bytes_per_cluster > size())
        return nullptr;

    return data() + offset;
}
