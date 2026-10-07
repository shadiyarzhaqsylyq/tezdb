/* Record Header at Payload
Header
Keys Array: [key 0 | key 1 | key 2 | key 3 | key 4]
Offsets Array: [offset 0 | offset 1 | offset 2 | offset 3 | offset 4]

Payload Area:
... [RecordHeader | Row 1 bytes] [RecordHeader | Row 2 bytes]





### Alternative
Header
Keys Array: [key 0 | key 1 | key 2 | key 3 | key 4]
Offsets Array: [Slot 0 | Slot 1 | Slot 2 | Slot 3 | Slot 4]

Payload Area:
... [Raw Row 1 bytes] [Raw Row 2 bytes]

*/

package db
import "core:mem"
import "core:slice"

PAGE_SIZE :: 16384 // 16 KB page

Page_Header :: struct #packed {
    page_lsn:     u64,  // 8 bytes
    page_id:      u32,  // 4 bytes
    next_page:    u32,  // 4 bytes
    prev_page:    u32,  // 4 bytes
    item_count:   u16,  // 2 bytes
    free_top:     u16,  // 2 bytes
    free_bottom:  u16,  // 2 bytes
    flags:        u16,  // 2 bytes
    reserved:     u32,  // 4 bytes <-- Added to pad exactly to 32 bytes, automatically zero-initialized to 0 by default
}

Slot :: bit_field u32 {
    lp_off:   u16 | 15, // Offset inside page (0..32,767)
    lp_flags: u8  | 2,  // 0: Unused, 1: Normal, 2: Deleted, 3: Overflow
    lp_len:   u16 | 15, // Exact payload length (0..32,767)
}


// We treat the raw page as a fixed-size byte buffer
Page :: struct #align(64) {
    data: [PAGE_SIZE]u8,
}


RecordHeader :: bit_field u16 {
    len:   u16 | 14, // Bits 0..13 (Row length up to 16,384 bytes = 16 KB)
    flags: u8  | 2,  // Bits 14..15 (0 = Normal, 1 = Deleted, 2 = Overflow)
}





