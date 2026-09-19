#include "Pager.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>

Pager::Pager(const std::string& filename) {
    fd_ = open(filename.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd_ == -1) {
        throw std::runtime_error("Unable to open file: " + filename);
    }

    struct stat st{};
    if (fstat(fd_, &st) == -1) {
        close(fd_);
        throw std::runtime_error("Error obtaining file stats: " + std::to_string(errno));
    }

    file_length_ = static_cast<uint32_t>(st.st_size);
    num_pages_ = file_length_ / PAGE_SIZE;

    if (file_length_ % PAGE_SIZE != 0) {
        close(fd_);
        throw std::runtime_error("DB file is not a whole number of pages. Corrupt file.");
    }
}

Pager::~Pager() {
    if (fd_ != -1) {
        close(fd_);
    }
}

Pager::Pager(Pager&& other) noexcept 
    : fd_(other.fd_), file_length_(other.file_length_), num_pages_(other.num_pages_) {
    pages_ = std::move(other.pages_);
    other.fd_ = -1;
}

Pager& Pager::operator=(Pager&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) close(fd_);
        fd_ = other.fd_;
        file_length_ = other.file_length_;
        num_pages_ = other.num_pages_;
        pages_ = std::move(other.pages_);
        other.fd_ = -1;
    }
    return *this;
}

uint8_t* Pager::get_page(uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        throw std::out_of_range("Page number out of bounds: " + std::to_string(page_num));
    }

    if (!pages_[page_num]) {
        auto page = std::make_unique<uint8_t[]>(PAGE_SIZE);
        uint32_t npages = file_length_ / PAGE_SIZE;
        if (file_length_ % PAGE_SIZE) npages += 1;

        if (page_num <= npages) {
            off_t offset = static_cast<off_t>(page_num) * PAGE_SIZE;
            ssize_t bytes_read = pread(fd_, page.get(), PAGE_SIZE, offset);
            if (bytes_read == -1) {
                throw std::runtime_error("Error reading file: " + std::to_string(errno));
            }
        }
        pages_[page_num] = std::move(page);
        if (page_num >= num_pages_) {
            num_pages_ = page_num + 1;
        }
    }
    return pages_[page_num].get();
}

void Pager::flush(uint32_t page_num) {
    if (!pages_[page_num]) {
        throw std::runtime_error("Tried to flush null page " + std::to_string(page_num));
    }
    off_t offset = static_cast<off_t>(page_num) * PAGE_SIZE;
    ssize_t bytes_written = pwrite(fd_, pages_[page_num].get(), PAGE_SIZE, offset);
    if (bytes_written == -1) {
        throw std::runtime_error("Error writing page: " + std::to_string(errno));
    }
}

void Pager::flush_all() {
    for (uint32_t i = 0; i < num_pages_; ++i) {
        if (pages_[i]) {
            flush(i);
        }
    }
}
