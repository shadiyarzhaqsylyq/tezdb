/* Record Header at Payload
Header
Keys Array: [key 0 | key 1 | key 2 | key 3 | key 4]
Offsets Array: [offset 0 | offset 1 | offset 2 | offset 3 | offset 4]

Payload Area:
... [RecordHeader | Row 1 bytes] [RecordHeader | Row 2 bytes]

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



### Alternative
Header
Keys Array: [key 0 | key 1 | key 2 | key 3 | key 4]
Offsets Array: [Slot 0 | Slot 1 | Slot 2 | Slot 3 | Slot 4]

Payload Area:
... [Raw Row 1 bytes] [Raw Row 2 bytes]

Slot :: bit_field u32 {
    lp_off:   u16 | 15, // Offset inside page (0..32,767)
    lp_flags: u8  | 2,  // 0: Unused, 1: Normal, 2: Deleted, 3: Overflow
    lp_len:   u16 | 15, // Exact payload length (0..32,767)
}
*/

package db
import "core:mem"
import "core:slice"

PAGE_SIZE :: 16384 // 16 KB page (standard for InnoDB-like engines)

Page_Header :: struct #packed {
    page_lsn:     u64,  // Log Sequence Number for write-ahead logging (WAL)
    page_id:      u32,
    next_page:    u32,
    prev_page:    u32,
    item_count:   u16,
    free_top:     u16,  // Grows downward (offset from start of page)
    free_bottom:  u16,  // Grows upward (offset from start of page)
    flags:        u16,
}

// We treat the raw page as a fixed-size byte buffer
Page :: struct #align(64) {
    data: [PAGE_SIZE]u8,
}


RecordHeader :: bit_field u16 {
    len:   u16 | 14, // Bits 0..13 (Row length up to 16,384 bytes = 16 KB)
    flags: u8  | 2,  // Bits 14..15 (0 = Normal, 1 = Deleted, 2 = Overflow)
}





