#pragma once

#include "Types.hpp"
#include <array>
#include <memory>
#include <vector>
#include <string>

class Pager {
public:
    explicit Pager(const std::string& filename);
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;
    Pager(Pager&&) noexcept;
    Pager& operator=(Pager&&) noexcept;

    uint8_t* get_page(uint32_t page_num);
    void flush(uint32_t page_num);
    void flush_all();

    [[nodiscard]] uint32_t num_pages() const noexcept { return num_pages_; }
    void set_num_pages(uint32_t pages) noexcept { num_pages_ = pages; }

    [[nodiscard]] bool has_page(uint32_t page_num) const noexcept {
        return page_num < TABLE_MAX_PAGES && pages_[page_num] != nullptr;
    }

    void drop_page(uint32_t page_num) {
        if (page_num < TABLE_MAX_PAGES) {
            pages_[page_num].reset();
        }
    }

private:
    int fd_{-1};
    uint32_t file_length_{0};
    uint32_t num_pages_{0};
    std::array<std::unique_ptr<uint8_t[]>, TABLE_MAX_PAGES> pages_{};
};
