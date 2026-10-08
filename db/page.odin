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



Current
Header
[Slot 0][Slot 1][Slot 2] goes downward

Free Space
Size = upper - lower

goes upward
Row 2 [ Header + Key + Value]
Row 1 [ Header + Key + Value]
Row 0 [ Header + Key + Value]

Row Header
we have 5 columns
Bitmap 0 1 0 1 0
1st columns is null
2nd columns is not null


*/

package db
import "core:mem"
import "core:slice"

PAGE_SIZE :: 8192 // 8 KB page


// Alternative
Page_Header :: struct #packed {
    // 1. 8-Byte Fields (Offsets 0 .. 7)
    lsn:              u64,

    // 2. 4-Byte Fields (Offsets 8 .. 15)
    page_id:          u32,
    checksum:         u32,

    // 3. 2-Byte Fields (Offsets 16 .. 23)
    lower:            u16,
    upper:            u16,
    slot_count:       u16,
    fragmented_space: u16,

    // 4. 1-Byte Fields (Offsets 24 .. 25)
    page_type:        u8,
    flags:            u8,

    // 5. Explicit Reserved Padding to reach exactly 32 bytes (Offsets 26 .. 31)
    reserved:         [6]u8, // Must be initialized to 0!
} // 32 byte





/* 8byte alternative
Slot :: struct #packed {
    prefix: u32,       // First 4 bytes of the key (Big-Endian)
    length: u16,       // Length of the row payload (up to 64 KB)
    using info: bit_field u16 {
        offset: u16 | 14, // Byte offset to payload (up to 16,384 bytes)
        flag:   u8  | 2,  // 00=Dead, 01=Live, 10=Redirected
    },
}
*/

// We treat the raw page as a fixed-size byte buffer
Page :: struct #align(64) {
    data: [PAGE_SIZE]u8,
}

Page :: struct #packed {
    header: Page_Header,
    data:   [PAGE_SIZE - size_of(Page_Header)]u8,
}





