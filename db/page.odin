/* Record Header at Payload
Header
Keys Array: [key 0 | key 1 | key 2 | key 3 | key 4]
Offsets Array: [offset 0 | offset 1 | offset 2 | offset 3 | offset 4]

Payload Area:
... [RecordHeader | Row 1 bytes] [RecordHeader | Row 2 bytes]
*/

package db
import "core:mem"
import "core:slice"

PAGE_SIZE :: 16384 // 16 KB page (standard for InnoDB-like engines)

Page_Header :: struct #packed {
    page_lsn:     u64,  // Log Sequence Number for write-ahead logging (WAL)
    page_id:      u32,
    item_count:   u16,
    free_top:     u16,  // Grows downward (offset from start of page)
    free_bottom:  u16,  // Grows upward (offset from start of page)
    flags:        u16,
}

// We treat the raw page as a fixed-size byte buffer
Page :: struct #align(64) {
    data: [PAGE_SIZE]u8,
}

page_get_header :: proc(page: ^Page) -> ^Page_Header {
    return cast(^Page_Header)&page.data[0]
}

// Get the contiguous keys array for fast SIMD/binary search
page_get_keys :: proc(page: ^Page, $Key_Type: typeid) -> []Key_Type {
    header := page_get_header(page)
    offset := size_of(Page_Header)
    ptr := cast(^Key_Type)&page.data[offset]
    return slice.from_ptr(ptr, int(header.item_count))
}

// Get the offsets array
page_get_offsets :: proc(page: ^Page, $Key_Type: typeid) -> []u16 {
    header := page_get_header(page)
    offset := size_of(Page_Header) + (int(header.item_count) * size_of(Key_Type))
    ptr := cast(^u16)&page.data[offset]
    return slice.from_ptr(ptr, int(header.item_count))
}

// Binary search is extremely fast because it's scanning contiguous memory
page_find_key :: proc(page: ^Page, key: i64) -> (payload_offset: u16, found: bool) {
    keys := page_get_keys(page, i64)
    idx, ok := slice.binary_search(keys, key)
    if !ok do return 0, false

    offsets := page_get_offsets(page, i64)
    return offsets[idx], true
}


// Exactly 2 bytes prepended to every record payload
RecordHeader :: bit_field u16 {
    len:   u16 | 14, // Bits 0..13 (Row length up to 16,384 bytes = 16 KB)
    flags: u8  | 2,  // Bits 14..15 (0 = Normal, 1 = Deleted, 2 = Overflow)
}

RecordFlag :: enum u8 {
    Normal   = 0,
    Deleted  = 1, // Tombstone (marked deleted without shifting data)
    Overflow = 2, // Row is large and spills to an overflow page
}


