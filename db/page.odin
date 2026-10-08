package db
import "core:mem"
import "core:slice"

PAGE_SIZE :: 8192 // 8 KB page



Page_Header :: struct #packed {

}


// Exactly 32 bits (4 bytes)
Slot :: bit_field u32 {
    offset:   u16  | 16, // 16 bits: Supports page sizes up to 64 KB (0..65535)
    length:   u16  | 13, // 13 bits: Max inline row length is 8191 bytes
    deleted:  bool |  1, // Tombstone marker
    overflow: bool |  1, // Set if row has off-page BLOB/overflow columns
    redirect: bool |  1, // Set if row is forwarded
}

#assert(size_of(Slot) == 4, "Slot must be exactly 4 bytes")

/* Alternative
Slot_Meta :: bit_field u32 {
    offset:   u16  | 16,
    length:   u16  | 13,
    deleted:  bool |  1,
    overflow: bool |  1,
    redirect: bool |  1,
}

Slot :: struct #packed {
    key_prefix: u32,       // 4 bytes: First 4 bytes of Primary Key for fast SIMD/binary search
    meta:       Slot_Meta, // 4 bytes: Offset, Length, Flags
}

#assert(size_of(Slot) == 8, "Slot must be exactly 8 bytes (64-bit word)")
*/


// We treat the raw page as a fixed-size byte buffer
Page :: struct #align(64) {
    data: [PAGE_SIZE]u8,
}

Page :: struct #packed {
    header: Page_Header,
    data:   [PAGE_SIZE - size_of(Page_Header)]u8,
}





