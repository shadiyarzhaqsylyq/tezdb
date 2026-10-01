package main

import "core:fmt"
import "core:os"
import "core:mem"
import "core:strings"
import "core:strconv"
import "core:bufio"
import "core:slice"

// ============================================================================
// Data Types & Schema Definition
// ============================================================================

PAGE_SIZE                  :: 4096
TABLE_MAX_PAGES            :: 400
INVALID_PAGE_NUM           :: max(u32)
INSERT_PAGE_SAFETY_MARGIN  :: 8
SCHEMA_MAGIC               :: 0x5343484D

MAX_COLUMNS     :: 32
MAX_NAME_LEN    :: 64
MAX_STR_LEN     :: 256
MAX_TOKENS      :: 256
MAX_ASSIGNMENTS :: 32

Data_Type :: enum i32 {
	Int     = 0,
	Varchar = 1,
}

Column_Def :: struct {
	name:           [MAX_NAME_LEN]u8,
	type:           Data_Type,
	length:         u32, // For VARCHAR length; 0 for INT
	offset:         u32, // Byte offset in row buffer
	size:           u32, // Size in row buffer
	is_primary_key: bool,
}

Schema :: struct {
	has_schema:         bool,
	table_name:         [MAX_NAME_LEN]u8,
	columns:            [MAX_COLUMNS]Column_Def,
	num_columns:        u32,
	row_size:           u32,
	primary_key_index:  u32,
}

// ----------------------------------------------------------------------
// Small helpers for fixed-size byte-buffer "C strings"
// ----------------------------------------------------------------------

buf_set :: proc(dst: []u8, src: string) {
	n := len(src)
	if n > len(dst) - 1 {
		n = len(dst) - 1
	}
	if n > 0 {
		copy(dst[:n], src[:n])
	}
	for i := n; i < len(dst); i += 1 {
		dst[i] = 0
	}
}

buf_str :: proc(buf: []u8) -> string {
	n := 0
	for n < len(buf) && buf[n] != 0 {
		n += 1
	}
	return string(buf[:n])
}

schema_add_column :: proc(schema: ^Schema, name: string, type: Data_Type, length: u32, is_pk: bool) {
	if schema.num_columns >= MAX_COLUMNS do return

	col := &schema.columns[schema.num_columns]
	buf_set(col.name[:], name)
	col.type = type
	col.is_primary_key = is_pk
	col.offset = schema.row_size

	if type == .Int {
		col.length = 0
		col.size = size_of(i32)
	} else { // VARCHAR
		col.length = length > 0 ? length : 32
		col.size = col.length + 1 // Null-terminated string buffer
	}

	if is_pk {
		schema.primary_key_index = schema.num_columns
	}

	schema.row_size += col.size
	schema.num_columns += 1
	schema.has_schema = true
}

schema_find_column :: proc(schema: ^Schema, name: string) -> int {
	for i in 0 ..< schema.num_columns {
		if strings.equal_fold(buf_str(schema.columns[i].name[:]), name) {
			return int(i)
		}
	}
	return -1
}

// ============================================================================
// Dynamic Value & Row Structures
// ============================================================================

Value :: struct {
	type:    Data_Type,
	int_val: i32,
	str_val: [MAX_STR_LEN]u8,
}

Dynamic_Row :: struct {
	values:     [MAX_COLUMNS]Value,
	num_values: u32,
}

get_pk_value :: proc(row: ^Dynamic_Row, schema: ^Schema) -> u32 {
	if schema.primary_key_index < row.num_values {
		return u32(row.values[schema.primary_key_index].int_val)
	}
	return 0
}

Execute_Result :: enum {
	Success,
	Duplicate_Key,
	Not_Found,
	Tx_Already_Active,
	No_Active_Tx,
	Table_Full,
	Catalog_Full,
}

Meta_Command_Result :: enum {
	Success,
	Unrecognized_Command,
}

Prepare_Result :: enum {
	Success,
	Negative_Id,
	String_Too_Long,
	Syntax_Error,
	Unrecognized_Statement,
	Table_Not_Found,
	Table_Exists,
	Column_Exists,
	Column_Not_Found,
	Cannot_Drop_Primary_Key,
	Cannot_Drop_Last_Column,
	Too_Many_Columns,
	Order_By_Column_Not_Found,
}

Statement_Type :: enum {
	Insert,
	Select,
	Update,
	Delete,
	Create,
	Drop,
	Alter_Add_Column,
	Alter_Drop_Column,
	Begin,
	Commit,
	Rollback,
}

Where_Op :: enum {
	Eq,
	Neq,
	Gt,
	Lt,
	Ge,
	Le,
}

Node_Type :: enum u8 {
	Internal = 0,
	Leaf     = 1,
}

// ============================================================================
// Serialization Utilities
// ============================================================================

serialize_row :: proc(source: ^Dynamic_Row, destination: rawptr, schema: ^Schema) {
	mem.set(destination, 0, int(schema.row_size))
	for i in 0 ..< schema.num_columns {
		col := &schema.columns[i]
		dest_ptr := cast(^u8)(uintptr(destination) + uintptr(col.offset))
		if i < source.num_values {
			val := &source.values[i]
			if col.type == .Int {
				v := val.int_val
				mem.copy(dest_ptr, &v, size_of(i32))
			} else {
				s := buf_str(val.str_val[:])
				n := len(s)
				if n > int(col.length) do n = int(col.length)
				if n > 0 {
					mem.copy(dest_ptr, raw_data(s), n)
				}
				(cast(^u8)(uintptr(dest_ptr) + uintptr(col.length)))^ = 0
			}
		}
	}
}

deserialize_row :: proc(source: rawptr, destination: ^Dynamic_Row, schema: ^Schema) {
	destination.num_values = schema.num_columns
	for i in 0 ..< schema.num_columns {
		col := &schema.columns[i]
		src_ptr := cast(^u8)(uintptr(source) + uintptr(col.offset))
		if col.type == .Int {
			v: i32
			mem.copy(&v, src_ptr, size_of(i32))
			destination.values[i].type = .Int
			destination.values[i].int_val = v
		} else {
			destination.values[i].type = .Varchar
			n := int(col.size)
			if n > MAX_STR_LEN do n = MAX_STR_LEN
			mem.copy(&destination.values[i].str_val[0], src_ptr, n)
			destination.values[i].str_val[col.size - 1] = 0
		}
	}
}

print_row :: proc(row: ^Dynamic_Row, schema: ^Schema) {
	fmt.print("(")
	for i in 0 ..< schema.num_columns {
		if i > 0 do fmt.print(", ")
		if schema.columns[i].type == .Int {
			fmt.print(row.values[i].int_val)
		} else {
			fmt.printf("'%s'", buf_str(row.values[i].str_val[:]))
		}
	}
	fmt.print(")\n")
}

// ============================================================================
// WHERE-clause Structures & Evaluation (Tree with AND / OR / Parentheses)
// ============================================================================

Expr_Type :: enum {
	Comparison,
	Logical,
}

Logical_Op :: enum {
	And,
	Or,
}

Literal :: struct {
	is_string: bool,
	int_value: u32,
	str_value: [MAX_STR_LEN]u8,
}

Expr :: struct {
	type: Expr_Type,

	// EXPR_COMPARISON fields
	column: [MAX_NAME_LEN]u8,
	op:     Where_Op,
	value:  Literal,

	// EXPR_LOGICAL fields
	log_op: Logical_Op,
	left:   ^Expr,
	right:  ^Expr,
}

free_expr :: proc(expr: ^Expr) {
	if expr == nil do return
	if expr.type == .Logical {
		free_expr(expr.left)
		free_expr(expr.right)
	}
	free(expr)
}

evaluate_expr :: proc(expr: ^Expr, row: ^Dynamic_Row, schema: ^Schema) -> bool {
	if expr == nil do return true

	if expr.type == .Logical {
		if expr.log_op == .And {
			return evaluate_expr(expr.left, row, schema) && evaluate_expr(expr.right, row, schema)
		} else if expr.log_op == .Or {
			return evaluate_expr(expr.left, row, schema) || evaluate_expr(expr.right, row, schema)
		}
		return false
	}

	col_idx := schema_find_column(schema, buf_str(expr.column[:]))
	if col_idx < 0 || u32(col_idx) >= row.num_values do return false

	col_def := &schema.columns[col_idx]
	cell_val := &row.values[col_idx]

	cmp := 0
	if col_def.type == .Int {
		v := i32(expr.value.int_value)
		if cell_val.int_val > v {
			cmp = 1
		} else if cell_val.int_val < v {
			cmp = -1
		}
	} else {
		cmp = strings.compare(buf_str(cell_val.str_val[:]), buf_str(expr.value.str_value[:]))
	}

	switch expr.op {
	case .Eq:
		return cmp == 0
	case .Neq:
		return cmp != 0
	case .Gt:
		return cmp > 0
	case .Lt:
		return cmp < 0
	case .Ge:
		return cmp >= 0
	case .Le:
		return cmp <= 0
	}
	return false
}

Update_Assignment :: struct {
	column_name: [MAX_NAME_LEN]u8,
	value_text:  [MAX_STR_LEN]u8,
}

Statement :: struct {
	type:           Statement_Type,
	row_to_insert:  Dynamic_Row,
	table_name:     [MAX_NAME_LEN]u8,
	created_schema: Schema,

	// The table this statement targets, resolved by prepare_statement via
	// the Database's catalog. nil for CREATE/BEGIN/COMMIT/ROLLBACK.
	resolved_table: ^Table,

	where_clause: ^Expr,
	is_count:     bool,
	target_id:    u32,

	// SELECT ... ORDER BY / LIMIT / OFFSET
	has_order_by:    bool,
	order_by_column: [MAX_NAME_LEN]u8,
	order_by_desc:   bool,
	has_limit:       bool,
	limit:           u32,
	offset:          u32,

	// SET-style UPDATE
	is_set_update:           bool,
	update_assignments:      [MAX_ASSIGNMENTS]Update_Assignment,
	num_update_assignments:  u32,

	// ALTER TABLE ADD/DROP COLUMN
	alter_column_name:   [MAX_NAME_LEN]u8,
	alter_column_type:   Data_Type,
	alter_column_length: u32,
	alter_default:       Value,
}

// ============================================================================
// B+Tree Layout Helpers
// ============================================================================

NODE_TYPE_SIZE      :: uintptr(size_of(u8))
NODE_TYPE_OFFSET    :: uintptr(0)
IS_ROOT_SIZE        :: uintptr(size_of(u8))
IS_ROOT_OFFSET      :: NODE_TYPE_OFFSET + NODE_TYPE_SIZE
PARENT_POINTER_SIZE :: uintptr(size_of(u32))
PARENT_POINTER_OFFSET :: IS_ROOT_OFFSET + IS_ROOT_SIZE
COMMON_NODE_HEADER_SIZE :: NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE

INTERNAL_NODE_NUM_KEYS_SIZE   :: uintptr(size_of(u32))
INTERNAL_NODE_NUM_KEYS_OFFSET :: COMMON_NODE_HEADER_SIZE
INTERNAL_NODE_RIGHT_CHILD_SIZE   :: uintptr(size_of(u32))
INTERNAL_NODE_RIGHT_CHILD_OFFSET :: INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE
INTERNAL_NODE_HEADER_SIZE :: COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE

INTERNAL_NODE_KEY_SIZE   :: uintptr(size_of(u32))
INTERNAL_NODE_CHILD_SIZE :: uintptr(size_of(u32))
INTERNAL_NODE_CELL_SIZE  :: INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE
INTERNAL_NODE_MAX_KEYS   :: 3

LEAF_NODE_NUM_CELLS_SIZE   :: uintptr(size_of(u32))
LEAF_NODE_NUM_CELLS_OFFSET :: COMMON_NODE_HEADER_SIZE
LEAF_NODE_NEXT_LEAF_SIZE   :: uintptr(size_of(u32))
LEAF_NODE_NEXT_LEAF_OFFSET :: LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE
LEAF_NODE_HEADER_SIZE      :: COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE
LEAF_NODE_KEY_SIZE         :: uintptr(size_of(u32))

node_byte_ptr :: proc(node: rawptr, offset: uintptr) -> ^u8 {
	return cast(^u8)(uintptr(node) + offset)
}

node_u32_ptr :: proc(node: rawptr, offset: uintptr) -> ^u32 {
	return cast(^u32)(uintptr(node) + offset)
}

get_node_type :: proc(node: rawptr) -> Node_Type {
	return Node_Type(node_byte_ptr(node, NODE_TYPE_OFFSET)^)
}

set_node_type :: proc(node: rawptr, type: Node_Type) {
	node_byte_ptr(node, NODE_TYPE_OFFSET)^ = u8(type)
}

is_node_root :: proc(node: rawptr) -> bool {
	return node_byte_ptr(node, IS_ROOT_OFFSET)^ != 0
}

set_node_root :: proc(node: rawptr, is_root: bool) {
	node_byte_ptr(node, IS_ROOT_OFFSET)^ = is_root ? 1 : 0
}

node_parent :: proc(node: rawptr) -> ^u32 {
	return node_u32_ptr(node, PARENT_POINTER_OFFSET)
}

internal_node_num_keys :: proc(node: rawptr) -> ^u32 {
	return node_u32_ptr(node, INTERNAL_NODE_NUM_KEYS_OFFSET)
}

internal_node_right_child :: proc(node: rawptr) -> ^u32 {
	return node_u32_ptr(node, INTERNAL_NODE_RIGHT_CHILD_OFFSET)
}

internal_node_cell :: proc(node: rawptr, cell_num: u32) -> ^u32 {
	return node_u32_ptr(node, INTERNAL_NODE_HEADER_SIZE + uintptr(cell_num) * INTERNAL_NODE_CELL_SIZE)
}

internal_node_child :: proc(node: rawptr, child_num: u32) -> ^u32 {
	num_keys := internal_node_num_keys(node)^
	if child_num > num_keys {
		fmt.printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys)
		os.exit(1)
	} else if child_num == num_keys {
		right_child := internal_node_right_child(node)
		if right_child^ == INVALID_PAGE_NUM {
			fmt.println("Tried to access right child of node, but was invalid page")
			os.exit(1)
		}
		return right_child
	} else {
		child := internal_node_cell(node, child_num)
		if child^ == INVALID_PAGE_NUM {
			fmt.printf("Tried to access child %d of node, but was invalid page\n", child_num)
			os.exit(1)
		}
		return child
	}
	return nil
}

internal_node_key :: proc(node: rawptr, key_num: u32) -> ^u32 {
	return node_u32_ptr(rawptr(internal_node_cell(node, key_num)), INTERNAL_NODE_CHILD_SIZE)
}

leaf_node_cell_size :: proc(row_size: u32) -> u32 {
	return u32(LEAF_NODE_KEY_SIZE) + row_size
}

leaf_node_max_cells :: proc(row_size: u32) -> u32 {
	if row_size == 0 do return 0
	space := u32(PAGE_SIZE) - u32(LEAF_NODE_HEADER_SIZE)
	return space / leaf_node_cell_size(row_size)
}

leaf_node_num_cells :: proc(node: rawptr) -> ^u32 {
	return node_u32_ptr(node, LEAF_NODE_NUM_CELLS_OFFSET)
}

leaf_node_next_leaf :: proc(node: rawptr) -> ^u32 {
	return node_u32_ptr(node, LEAF_NODE_NEXT_LEAF_OFFSET)
}

leaf_node_cell :: proc(node: rawptr, cell_num: u32, row_size: u32) -> rawptr {
	return rawptr(node_byte_ptr(node, LEAF_NODE_HEADER_SIZE + uintptr(cell_num) * uintptr(leaf_node_cell_size(row_size))))
}

leaf_node_key :: proc(node: rawptr, cell_num: u32, row_size: u32) -> ^u32 {
	return cast(^u32)leaf_node_cell(node, cell_num, row_size)
}

leaf_node_value :: proc(node: rawptr, cell_num: u32, row_size: u32) -> rawptr {
	return rawptr(node_byte_ptr(leaf_node_cell(node, cell_num, row_size), LEAF_NODE_KEY_SIZE))
}

// ============================================================================
// Pager Structure & Functions
// ============================================================================

Pager :: struct {
	file:        ^os.File,
	file_length: u32,
	num_pages:   u32,
	pages:       [TABLE_MAX_PAGES]rawptr,
}

pager_open :: proc(filename: string) -> ^Pager {
	file, err := os.open(filename, os.O_RDWR | os.O_CREATE)
	if err != nil {
		fmt.println("Unable to open file")
		os.exit(1)
	}

	size, serr := os.file_size(file)
	if serr != nil {
		fmt.printf("Error obtaining file stats: %v\n", serr)
		os.exit(1)
	}

	pager := new(Pager)
	pager.file = file
	pager.file_length = u32(size)
	pager.num_pages = pager.file_length / PAGE_SIZE

	if pager.file_length % PAGE_SIZE != 0 {
		fmt.println("Db file is not a whole number of pages. Corrupt file.")
		os.exit(1)
	}

	return pager
}

pager_get_page :: proc(pager: ^Pager, page_num: u32) -> rawptr {
	if page_num >= TABLE_MAX_PAGES {
		fmt.printf("Tried to fetch page number out of bounds. %d >= %d\n", page_num, TABLE_MAX_PAGES)
		os.exit(1)
	}

	if pager.pages[page_num] == nil {
		page, _ := mem.alloc(PAGE_SIZE)
		npages := pager.file_length / PAGE_SIZE
		if pager.file_length % PAGE_SIZE != 0 {
			npages += 1
		}
		if page_num <= npages {
			buf := (cast([^]u8)page)[:PAGE_SIZE]
			offset := i64(page_num) * PAGE_SIZE
			os.read_at(pager.file, buf, offset)
		}
		pager.pages[page_num] = page
		if page_num >= pager.num_pages {
			pager.num_pages = page_num + 1
		}
	}
	return pager.pages[page_num]
}

pager_flush :: proc(pager: ^Pager, page_num: u32) {
	if pager.pages[page_num] == nil {
		fmt.println("Tried to flush null page")
		os.exit(1)
	}

	buf := (cast([^]u8)pager.pages[page_num])[:PAGE_SIZE]
	offset := i64(page_num) * PAGE_SIZE
	_, werr := os.write_at(pager.file, buf, offset)
	if werr != nil {
		fmt.printf("Error writing: %v\n", werr)
		os.exit(1)
	}
}

pager_close :: proc(pager: ^Pager) {
	for i in 0 ..< TABLE_MAX_PAGES {
		if pager.pages[i] != nil {
			mem.free(pager.pages[i])
			pager.pages[i] = nil
		}
	}
	os.close(pager.file)
	free(pager)
}

// ============================================================================
// Table & Cursor Structures & B+Tree Operations
// ============================================================================

MAX_TABLES    :: 16
CATALOG_MAGIC :: 0x43415441 // "CATA"

Table :: struct {
	pager:         ^Pager, // shared with all tables in the owning Database
	root_page_num: u32,
	schema:        Schema,
}

Cursor :: struct {
	table:        ^Table,
	page_num:     u32,
	cell_num:     u32,
	end_of_table: bool,
}

initialize_leaf_node :: proc(node: rawptr) {
	set_node_type(node, .Leaf)
	set_node_root(node, false)
	leaf_node_num_cells(node)^ = 0
	leaf_node_next_leaf(node)^ = 0
}

initialize_internal_node :: proc(node: rawptr) {
	set_node_type(node, .Internal)
	set_node_root(node, false)
	internal_node_num_keys(node)^ = 0
	internal_node_right_child(node)^ = INVALID_PAGE_NUM
}

// ============================================================================
// Database: owns the shared Pager and a catalog of Tables.
//
// On-disk layout:
//   page 0            -> Catalog_Header (magic, table count, the page number
//                         of each table's catalog entry, and a free-page list)
//   catalog entry page -> Table_Catalog_Entry (that table's root page + schema)
//   all other pages    -> ordinary B+tree node pages, shared out of the same
//                         page pool across every table in the file
// ============================================================================

MAX_FREE_PAGES :: TABLE_MAX_PAGES

Catalog_Header :: struct {
	magic:              u32,
	num_tables:         u32,
	catalog_page_nums:  [MAX_TABLES]u32,

	// Pages reclaimed from dropped tables, available for reuse before the
	// pager grows the file with a brand new page.
	free_count: u32,
	free_pages: [MAX_FREE_PAGES]u32,
}

Table_Catalog_Entry :: struct {
	root_page_num: u32,
	schema:        Schema,
}

Database :: struct {
	pager:      ^Pager,
	tables:     [MAX_TABLES]Table,
	num_tables: u32,

	// Transaction state (spans the whole file, not just one table)
	in_transaction:        bool,
	tx_original_num_pages: u32,
	tx_backup:             [TABLE_MAX_PAGES]rawptr,
	tx_was_cached:         [TABLE_MAX_PAGES]bool,
}

tx_free_backups :: proc(db: ^Database) {
	for i in 0 ..< TABLE_MAX_PAGES {
		if db.tx_backup[i] != nil {
			mem.free(db.tx_backup[i])
			db.tx_backup[i] = nil
		}
	}
}

db_find_table :: proc(db: ^Database, name: string) -> ^Table {
	for i in 0 ..< db.num_tables {
		if strings.equal_fold(buf_str(db.tables[i].schema.table_name[:]), name) {
			return &db.tables[i]
		}
	}
	return nil
}

// Re-derives db.tables/db.num_tables from whatever the catalog pages
// currently say. Used at open time, and after a ROLLBACK (which may have
// restored page 0 and any catalog-entry pages to their pre-transaction
// contents).
db_reload_catalog :: proc(db: ^Database) {
	db.num_tables = 0
	header := cast(^Catalog_Header)pager_get_page(db.pager, 0)
	if header.magic != CATALOG_MAGIC do return

	db.num_tables = header.num_tables
	for i in 0 ..< db.num_tables {
		entry := cast(^Table_Catalog_Entry)pager_get_page(db.pager, header.catalog_page_nums[i])
		db.tables[i] = Table{pager = db.pager, root_page_num = entry.root_page_num, schema = entry.schema}
	}
}

db_open :: proc(filename: string) -> ^Database {
	db := new(Database)
	db.pager = pager_open(filename)
	db.in_transaction = false

	if db.pager.num_pages == 0 {
		header := cast(^Catalog_Header)pager_get_page(db.pager, 0)
		header.magic = CATALOG_MAGIC
		header.num_tables = 0
		header.free_count = 0
	} else {
		db_reload_catalog(db)
	}
	return db
}

db_close :: proc(db: ^Database) {
	tx_free_backups(db)
	for i in 0 ..< db.pager.num_pages {
		if db.pager.pages[i] == nil do continue
		pager_flush(db.pager, i)
		mem.free(db.pager.pages[i])
		db.pager.pages[i] = nil
	}
	pager_close(db.pager)
	free(db)
}

// Returns a page number ready to hold new data: a reclaimed page from the
// free list if one is available, otherwise a brand new page at the end of
// the file. Either way the caller is responsible for fully overwriting the
// page's contents (initialize_leaf_node, a fresh catalog entry, etc.) since
// a reused page still holds whatever was on it before.
alloc_page :: proc(pager: ^Pager) -> u32 {
	header := cast(^Catalog_Header)pager_get_page(pager, 0)
	if header.magic == CATALOG_MAGIC && header.free_count > 0 {
		header.free_count -= 1
		return header.free_pages[header.free_count]
	}
	return pager.num_pages
}

// Returns a page to the free list so a later alloc_page can reuse it. If
// the free list is somehow already full (can't happen in practice: it's
// sized to hold every page in the file), the page is silently leaked
// rather than corrupting the list.
free_page :: proc(pager: ^Pager, page_num: u32) {
	header := cast(^Catalog_Header)pager_get_page(pager, 0)
	if int(header.free_count) < len(header.free_pages) {
		header.free_pages[header.free_count] = page_num
		header.free_count += 1
	}
}

// Walks a table's whole B+tree, collecting every page number it occupies
// (internal nodes and leaves alike) so they can be handed to free_page.
collect_table_pages :: proc(pager: ^Pager, page_num: u32, out: ^[dynamic]u32) {
	append(out, page_num)
	node := pager_get_page(pager, page_num)
	if get_node_type(node) == .Internal {
		num_keys := internal_node_num_keys(node)^
		for i in 0 ..< num_keys {
			collect_table_pages(pager, internal_node_child(node, i)^, out)
		}
		right := internal_node_right_child(node)^
		if right != INVALID_PAGE_NUM {
			collect_table_pages(pager, right, out)
		}
	}
}

// Creates a new table (and its root B+tree page + catalog entry) inside an
// already-open Database. Returns false if the table already exists or the
// catalog is full.
db_create_table :: proc(db: ^Database, schema: ^Schema) -> bool {
	name := buf_str(schema.table_name[:])
	if db.num_tables >= MAX_TABLES do return false
	if db_find_table(db, name) != nil do return false

	root_page_num := alloc_page(db.pager)
	root_node := pager_get_page(db.pager, root_page_num)
	initialize_leaf_node(root_node)
	set_node_root(root_node, true)

	catalog_entry_page_num := alloc_page(db.pager)
	entry := cast(^Table_Catalog_Entry)pager_get_page(db.pager, catalog_entry_page_num)
	entry.root_page_num = root_page_num
	entry.schema = schema^

	header := cast(^Catalog_Header)pager_get_page(db.pager, 0)
	header.magic = CATALOG_MAGIC
	header.catalog_page_nums[db.num_tables] = catalog_entry_page_num
	header.num_tables = db.num_tables + 1

	db.tables[db.num_tables] = Table{pager = db.pager, root_page_num = root_page_num, schema = schema^}
	db.num_tables += 1
	return true
}

// Drops a table by removing its entry from the catalog and reclaiming every
// page it used (its whole B+tree plus its catalog-entry page) onto the
// free list, so later CREATE TABLE / INSERT calls can reuse that space
// instead of growing the file.
db_drop_table :: proc(db: ^Database, name: string) -> bool {
	header := cast(^Catalog_Header)pager_get_page(db.pager, 0)
	if header.magic != CATALOG_MAGIC do return false

	found_idx := -1
	dropped_root: u32
	dropped_catalog_page: u32
	for i in 0 ..< header.num_tables {
		entry_page_num := header.catalog_page_nums[i]
		entry := cast(^Table_Catalog_Entry)pager_get_page(db.pager, entry_page_num)
		if strings.equal_fold(buf_str(entry.schema.table_name[:]), name) {
			found_idx = int(i)
			dropped_root = entry.root_page_num
			dropped_catalog_page = entry_page_num
			break
		}
	}
	if found_idx < 0 do return false

	for i := found_idx; i < int(header.num_tables) - 1; i += 1 {
		header.catalog_page_nums[i] = header.catalog_page_nums[i + 1]
	}
	header.num_tables -= 1

	pages := make([dynamic]u32, 0, 32)
	defer delete(pages)
	collect_table_pages(db.pager, dropped_root, &pages)
	for p in pages {
		free_page(db.pager, p)
	}
	free_page(db.pager, dropped_catalog_page)

	db_reload_catalog(db)
	return true
}

db_find_catalog_entry_page :: proc(db: ^Database, name: string) -> (u32, bool) {
	header := cast(^Catalog_Header)pager_get_page(db.pager, 0)
	if header.magic != CATALOG_MAGIC do return 0, false
	for i in 0 ..< header.num_tables {
		entry_page_num := header.catalog_page_nums[i]
		entry := cast(^Table_Catalog_Entry)pager_get_page(db.pager, entry_page_num)
		if strings.equal_fold(buf_str(entry.schema.table_name[:]), name) {
			return entry_page_num, true
		}
	}
	return 0, false
}

// Shared machinery for ALTER TABLE ADD/DROP COLUMN: both operations change
// every row's byte layout, so there's no way to edit rows in place. Instead
// this builds a brand new (empty) B+tree under new_schema, lets the caller
// migrate each old row into it via `build_row`, then swaps the table's
// catalog entry over to the new tree and reclaims every page the old tree
// used to occupy.
db_alter_rebuild :: proc(
	db: ^Database,
	table: ^Table,
	new_schema: Schema,
	build_row: proc(old_row: ^Dynamic_Row, old_schema: ^Schema, new_schema: ^Schema, extra: rawptr) -> Dynamic_Row,
	extra: rawptr,
) -> bool {
	old_root := table.root_page_num
	old_schema := table.schema
	name := buf_str(old_schema.table_name[:])

	new_root_page_num := alloc_page(db.pager)
	new_root_node := pager_get_page(db.pager, new_root_page_num)
	initialize_leaf_node(new_root_node)
	set_node_root(new_root_node, true)

	new_table := Table{pager = db.pager, root_page_num = new_root_page_num, schema = new_schema}
	old_table := Table{pager = db.pager, root_page_num = old_root, schema = old_schema}

	cursor := table_start(&old_table)
	old_row: Dynamic_Row
	for !cursor.end_of_table {
		deserialize_row(cursor_value(cursor), &old_row, &old_schema)

		new_row := build_row(&old_row, &old_schema, &new_table.schema, extra)
		key := get_pk_value(&new_row, &new_table.schema)

		ins_cursor := table_find(&new_table, key)
		leaf_node_insert(ins_cursor, key, &new_row)
		free(ins_cursor)

		cursor_advance(cursor)
	}
	free(cursor)

	pages := make([dynamic]u32, 0, 32)
	defer delete(pages)
	collect_table_pages(db.pager, old_root, &pages)
	for p in pages {
		free_page(db.pager, p)
	}

	entry_page_num, ok := db_find_catalog_entry_page(db, name)
	if !ok do return false
	entry := cast(^Table_Catalog_Entry)pager_get_page(db.pager, entry_page_num)
	entry.root_page_num = new_root_page_num
	entry.schema = new_table.schema

	table^ = new_table
	return true
}

Alter_Add_Extra :: struct {
	default_val: Value,
}

db_alter_add_column :: proc(db: ^Database, table: ^Table, col_name: string, col_type: Data_Type, col_len: u32, default_val: Value) -> bool {
	new_schema := table.schema
	schema_add_column(&new_schema, col_name, col_type, col_len, false)

	extra := Alter_Add_Extra{default_val = default_val}

	build :: proc(old_row: ^Dynamic_Row, old_schema: ^Schema, new_schema: ^Schema, extra_ptr: rawptr) -> Dynamic_Row {
		extra := cast(^Alter_Add_Extra)extra_ptr
		new_row: Dynamic_Row
		new_row.num_values = new_schema.num_columns
		for i in 0 ..< old_schema.num_columns {
			new_row.values[i] = old_row.values[i]
		}
		new_row.values[old_schema.num_columns] = extra.default_val
		return new_row
	}

	return db_alter_rebuild(db, table, new_schema, build, &extra)
}

Alter_Drop_Extra :: struct {
	drop_index: u32,
}

db_alter_drop_column :: proc(db: ^Database, table: ^Table, col_name: string) -> bool {
	old_schema := table.schema
	drop_idx := schema_find_column(&old_schema, col_name)
	if drop_idx < 0 do return false

	new_schema := Schema{}
	buf_set(new_schema.table_name[:], buf_str(old_schema.table_name[:]))
	for i in 0 ..< old_schema.num_columns {
		if i == u32(drop_idx) do continue
		col := &old_schema.columns[i]
		schema_add_column(&new_schema, buf_str(col.name[:]), col.type, col.length, col.is_primary_key)
	}

	extra := Alter_Drop_Extra{drop_index = u32(drop_idx)}

	build :: proc(old_row: ^Dynamic_Row, old_schema: ^Schema, new_schema: ^Schema, extra_ptr: rawptr) -> Dynamic_Row {
		extra := cast(^Alter_Drop_Extra)extra_ptr
		new_row: Dynamic_Row
		new_row.num_values = new_schema.num_columns
		out_i: u32 = 0
		for i in 0 ..< old_schema.num_columns {
			if i == extra.drop_index do continue
			new_row.values[out_i] = old_row.values[i]
			out_i += 1
		}
		return new_row
	}

	return db_alter_rebuild(db, table, new_schema, build, &extra)
}

get_node_max_key :: proc(table: ^Table, node: rawptr) -> u32 {
	if get_node_type(node) == .Leaf {
		return leaf_node_key(node, leaf_node_num_cells(node)^ - 1, table.schema.row_size)^
	}
	right_child := pager_get_page(table.pager, internal_node_right_child(node)^)
	return get_node_max_key(table, right_child)
}

leaf_node_find :: proc(table: ^Table, page_num: u32, key: u32) -> ^Cursor {
	node := pager_get_page(table.pager, page_num)
	num_cells := leaf_node_num_cells(node)^

	cursor := new(Cursor)
	cursor.table = table
	cursor.page_num = page_num
	cursor.end_of_table = false

	min_index: u32 = 0
	one_past_max_index := num_cells
	for one_past_max_index != min_index {
		index := (min_index + one_past_max_index) / 2
		key_at_index := leaf_node_key(node, index, table.schema.row_size)^
		if key == key_at_index {
			cursor.cell_num = index
			return cursor
		}
		if key < key_at_index {
			one_past_max_index = index
		} else {
			min_index = index + 1
		}
	}
	cursor.cell_num = min_index
	return cursor
}

internal_node_find_child :: proc(node: rawptr, key: u32) -> u32 {
	num_keys := internal_node_num_keys(node)^
	min_index: u32 = 0
	max_index := num_keys
	for min_index != max_index {
		index := (min_index + max_index) / 2
		key_to_right := internal_node_key(node, index)^
		if key_to_right >= key {
			max_index = index
		} else {
			min_index = index + 1
		}
	}
	return min_index
}

internal_node_find :: proc(table: ^Table, page_num: u32, key: u32) -> ^Cursor {
	node := pager_get_page(table.pager, page_num)
	child_index := internal_node_find_child(node, key)
	child_num := internal_node_child(node, child_index)^
	child := pager_get_page(table.pager, child_num)
	switch get_node_type(child) {
	case .Leaf:
		return leaf_node_find(table, child_num, key)
	case .Internal:
		return internal_node_find(table, child_num, key)
	}
	return nil
}

table_find :: proc(table: ^Table, key: u32) -> ^Cursor {
	root_node := pager_get_page(table.pager, table.root_page_num)
	if get_node_type(root_node) == .Leaf {
		return leaf_node_find(table, table.root_page_num, key)
	} else {
		return internal_node_find(table, table.root_page_num, key)
	}
}

table_start :: proc(table: ^Table) -> ^Cursor {
	cursor := table_find(table, 0)
	node := pager_get_page(table.pager, cursor.page_num)
	num_cells := leaf_node_num_cells(node)^
	cursor.end_of_table = (num_cells == 0)
	return cursor
}

cursor_value :: proc(cursor: ^Cursor) -> rawptr {
	page := pager_get_page(cursor.table.pager, cursor.page_num)
	return leaf_node_value(page, cursor.cell_num, cursor.table.schema.row_size)
}

cursor_advance :: proc(cursor: ^Cursor) {
	node := pager_get_page(cursor.table.pager, cursor.page_num)
	cursor.cell_num += 1
	if cursor.cell_num >= leaf_node_num_cells(node)^ {
		next_page_num := leaf_node_next_leaf(node)^
		if next_page_num == 0 {
			cursor.end_of_table = true
		} else {
			cursor.page_num = next_page_num
			cursor.cell_num = 0
		}
	}
}

leaf_node_delete :: proc(cursor: ^Cursor) {
	node := pager_get_page(cursor.table.pager, cursor.page_num)
	num_cells := leaf_node_num_cells(node)^
	cell_sz := leaf_node_cell_size(cursor.table.schema.row_size)
	for i := cursor.cell_num; i < num_cells - 1; i += 1 {
		mem.copy(
			leaf_node_cell(node, i, cursor.table.schema.row_size),
			leaf_node_cell(node, i + 1, cursor.table.schema.row_size),
			int(cell_sz),
		)
	}
	leaf_node_num_cells(node)^ -= 1
}

get_unused_page_num :: proc(table: ^Table) -> u32 {
	return alloc_page(table.pager)
}

create_new_root :: proc(table: ^Table, right_child_page_num: u32) {
	root := pager_get_page(table.pager, table.root_page_num)
	right_child := pager_get_page(table.pager, right_child_page_num)
	left_child_page_num := get_unused_page_num(table)
	left_child := pager_get_page(table.pager, left_child_page_num)

	if get_node_type(root) == .Internal {
		initialize_internal_node(right_child)
		initialize_internal_node(left_child)
	}

	mem.copy(left_child, root, PAGE_SIZE)
	set_node_root(left_child, false)

	if get_node_type(left_child) == .Internal {
		child: rawptr
		for i in 0 ..< internal_node_num_keys(left_child)^ {
			child = pager_get_page(table.pager, internal_node_child(left_child, i)^)
			node_parent(child)^ = left_child_page_num
		}
		child = pager_get_page(table.pager, internal_node_right_child(left_child)^)
		node_parent(child)^ = left_child_page_num
	}

	initialize_internal_node(root)
	set_node_root(root, true)
	internal_node_num_keys(root)^ = 1
	internal_node_child(root, 0)^ = left_child_page_num
	left_child_max_key := get_node_max_key(table, left_child)
	internal_node_key(root, 0)^ = left_child_max_key
	internal_node_right_child(root)^ = right_child_page_num
	node_parent(left_child)^ = table.root_page_num
	node_parent(right_child)^ = table.root_page_num
}

update_internal_node_key :: proc(node: rawptr, old_key: u32, new_key: u32) {
	old_child_index := internal_node_find_child(node, old_key)
	internal_node_key(node, old_child_index)^ = new_key
}

internal_node_insert :: proc(table: ^Table, parent_page_num: u32, child_page_num: u32) {
	parent := pager_get_page(table.pager, parent_page_num)
	child := pager_get_page(table.pager, child_page_num)
	child_max_key := get_node_max_key(table, child)
	index := internal_node_find_child(parent, child_max_key)

	original_num_keys := internal_node_num_keys(parent)^
	if original_num_keys >= INTERNAL_NODE_MAX_KEYS {
		internal_node_split_and_insert(table, parent_page_num, child_page_num)
		return
	}

	right_child_page_num := internal_node_right_child(parent)^
	if right_child_page_num == INVALID_PAGE_NUM {
		internal_node_right_child(parent)^ = child_page_num
		return
	}

	right_child := pager_get_page(table.pager, right_child_page_num)
	internal_node_num_keys(parent)^ = original_num_keys + 1

	if child_max_key > get_node_max_key(table, right_child) {
		internal_node_child(parent, original_num_keys)^ = right_child_page_num
		internal_node_key(parent, original_num_keys)^ = get_node_max_key(table, right_child)
		internal_node_right_child(parent)^ = child_page_num
	} else {
		for i := original_num_keys; i > index; i -= 1 {
			destination := internal_node_cell(parent, i)
			source := internal_node_cell(parent, i - 1)
			mem.copy(destination, source, int(INTERNAL_NODE_CELL_SIZE))
		}
		internal_node_child(parent, index)^ = child_page_num
		internal_node_key(parent, index)^ = child_max_key
	}
}

internal_node_split_and_insert :: proc(table: ^Table, parent_page_num: u32, child_page_num: u32) {
	old_page_num := parent_page_num
	old_node := pager_get_page(table.pager, parent_page_num)
	old_max := get_node_max_key(table, old_node)

	child := pager_get_page(table.pager, child_page_num)
	child_max := get_node_max_key(table, child)

	new_page_num := get_unused_page_num(table)

	splitting_root := is_node_root(old_node)
	parent: rawptr
	new_node: rawptr = nil

	if splitting_root {
		create_new_root(table, new_page_num)
		parent = pager_get_page(table.pager, table.root_page_num)
		old_page_num = internal_node_child(parent, 0)^
		old_node = pager_get_page(table.pager, old_page_num)
	} else {
		parent = pager_get_page(table.pager, node_parent(old_node)^)
		new_node = pager_get_page(table.pager, new_page_num)
		initialize_internal_node(new_node)
	}

	old_num_keys := internal_node_num_keys(old_node)
	cur_page_num := internal_node_right_child(old_node)^
	cur := pager_get_page(table.pager, cur_page_num)

	internal_node_insert(table, new_page_num, cur_page_num)
	node_parent(cur)^ = new_page_num
	internal_node_right_child(old_node)^ = INVALID_PAGE_NUM

	for i := i32(INTERNAL_NODE_MAX_KEYS) - 1; i > i32(INTERNAL_NODE_MAX_KEYS / 2); i -= 1 {
		cur_page_num = internal_node_child(old_node, u32(i))^
		cur = pager_get_page(table.pager, cur_page_num)
		internal_node_insert(table, new_page_num, cur_page_num)
		node_parent(cur)^ = new_page_num
		old_num_keys^ -= 1
	}

	internal_node_right_child(old_node)^ = internal_node_child(old_node, old_num_keys^ - 1)^
	old_num_keys^ -= 1

	max_after_split := get_node_max_key(table, old_node)
	destination_page_num := child_max < max_after_split ? old_page_num : new_page_num

	internal_node_insert(table, destination_page_num, child_page_num)
	node_parent(child)^ = destination_page_num

	update_internal_node_key(parent, old_max, get_node_max_key(table, old_node))

	if !splitting_root {
		internal_node_insert(table, node_parent(old_node)^, new_page_num)
		node_parent(new_node)^ = node_parent(old_node)^
	}
}

leaf_node_split_and_insert :: proc(cursor: ^Cursor, key: u32, value: ^Dynamic_Row) {
	table := cursor.table
	old_node := pager_get_page(table.pager, cursor.page_num)
	old_max := get_node_max_key(table, old_node)
	new_page_num := get_unused_page_num(table)
	new_node := pager_get_page(table.pager, new_page_num)

	initialize_leaf_node(new_node)
	node_parent(new_node)^ = node_parent(old_node)^
	leaf_node_next_leaf(new_node)^ = leaf_node_next_leaf(old_node)^
	leaf_node_next_leaf(old_node)^ = new_page_num

	max_cells := leaf_node_max_cells(table.schema.row_size)
	right_split_count := (max_cells + 1) / 2
	left_split_count := (max_cells + 1) - right_split_count
	cell_sz := leaf_node_cell_size(table.schema.row_size)

	for i := i32(max_cells); i >= 0; i -= 1 {
		destination_node: rawptr
		if u32(i) >= left_split_count {
			destination_node = new_node
		} else {
			destination_node = old_node
		}
		index_within_node := u32(i) % left_split_count
		destination := leaf_node_cell(destination_node, index_within_node, table.schema.row_size)

		if u32(i) == cursor.cell_num {
			serialize_row(value, leaf_node_value(destination_node, index_within_node, table.schema.row_size), &table.schema)
			leaf_node_key(destination_node, index_within_node, table.schema.row_size)^ = key
		} else if u32(i) > cursor.cell_num {
			mem.copy(destination, leaf_node_cell(old_node, u32(i) - 1, table.schema.row_size), int(cell_sz))
		} else {
			mem.copy(destination, leaf_node_cell(old_node, u32(i), table.schema.row_size), int(cell_sz))
		}
	}

	leaf_node_num_cells(old_node)^ = left_split_count
	leaf_node_num_cells(new_node)^ = right_split_count

	if is_node_root(old_node) {
		create_new_root(table, new_page_num)
	} else {
		parent_page_num := node_parent(old_node)^
		new_max := get_node_max_key(table, old_node)
		parent := pager_get_page(table.pager, parent_page_num)
		update_internal_node_key(parent, old_max, new_max)
		internal_node_insert(table, parent_page_num, new_page_num)
	}
}

leaf_node_insert :: proc(cursor: ^Cursor, key: u32, value: ^Dynamic_Row) {
	table := cursor.table
	node := pager_get_page(table.pager, cursor.page_num)
	num_cells := leaf_node_num_cells(node)^
	max_cells := leaf_node_max_cells(table.schema.row_size)

	if num_cells >= max_cells {
		leaf_node_split_and_insert(cursor, key, value)
		return
	}
	cell_sz := leaf_node_cell_size(table.schema.row_size)
	if cursor.cell_num < num_cells {
		for i := num_cells; i > cursor.cell_num; i -= 1 {
			mem.copy(
				leaf_node_cell(node, i, table.schema.row_size),
				leaf_node_cell(node, i - 1, table.schema.row_size),
				int(cell_sz),
			)
		}
	}
	leaf_node_num_cells(node)^ += 1
	leaf_node_key(node, cursor.cell_num, table.schema.row_size)^ = key
	serialize_row(value, leaf_node_value(node, cursor.cell_num, table.schema.row_size), &table.schema)
}

print_tree :: proc(table: ^Table, page_num: u32, indentation_level: u32) {
	node := pager_get_page(table.pager, page_num)
	num_keys, child: u32

	for i in 0 ..< indentation_level do fmt.print("  ")

	switch get_node_type(node) {
	case .Leaf:
		num_keys = leaf_node_num_cells(node)^
		fmt.printf("- leaf (size %d)\n", num_keys)
		for i in 0 ..< num_keys {
			for j in 0 ..< indentation_level + 1 do fmt.print("  ")
			fmt.printf("- %d\n", leaf_node_key(node, i, table.schema.row_size)^)
		}
	case .Internal:
		num_keys = internal_node_num_keys(node)^
		fmt.printf("- internal (size %d)\n", num_keys)
		if num_keys > 0 {
			for i in 0 ..< num_keys {
				child = internal_node_child(node, i)^
				print_tree(table, child, indentation_level + 1)
				for j in 0 ..< indentation_level + 1 do fmt.print("  ")
				fmt.printf("- key %d\n", internal_node_key(node, i)^)
			}
			child = internal_node_right_child(node)^
			print_tree(table, child, indentation_level + 1)
		}
	}
}


// ============================================================================
// SQL Lexer & Parser
// ============================================================================

Token_Kind :: enum {
	Identifier,
	String_Literal,
	Number,
	Symbol,
	End,
}

// NOTE: text is a string *slice into the original input line*, not an
// owned copy. This is safe because the input line outlives all tokenizing
// and parsing that happens against it within a single REPL iteration.
// (Earlier revision used a fixed [N]u8 buffer per token and a helper that
// copied through a local Token variable before returning a string into it;
// that string became a dangling reference the instant the helper returned,
// since the local Token's backing array was stack memory that had already
// gone out of scope. Slicing the stable input string avoids that class of
// bug entirely.)
Token :: struct {
	kind: Token_Kind,
	text: string,
}

Token_List :: struct {
	tokens: [MAX_TOKENS]Token,
	count:  u32,
	cursor: u32,
}

is_space :: proc(c: u8) -> bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'
}
is_digit :: proc(c: u8) -> bool {
	return c >= '0' && c <= '9'
}
is_alpha :: proc(c: u8) -> bool {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}
is_alnum :: proc(c: u8) -> bool {
	return is_alpha(c) || is_digit(c)
}

tokenize_input :: proc(input: string, list: ^Token_List) {
	list.count = 0
	list.cursor = 0
	pos := 0
	length := len(input)

	for pos < length && list.count < MAX_TOKENS - 1 {
		if is_space(input[pos]) {
			pos += 1
			continue
		}

		if input[pos] == ';' {
			list.tokens[list.count] = Token{kind = .Symbol, text = input[pos:pos + 1]}
			list.count += 1
			pos += 1
			continue
		}

		if input[pos] == '\'' {
			start := pos + 1
			pos += 1
			for pos < length && input[pos] != '\'' && pos - start < MAX_STR_LEN - 1 {
				pos += 1
			}
			str := input[start:pos]
			if pos < length && input[pos] == '\'' do pos += 1

			list.tokens[list.count] = Token{kind = .String_Literal, text = str}
			list.count += 1
			continue
		}

		// Two-character operators: <=, >=, !=, <>
		if pos + 1 < length {
			op2 := input[pos:pos + 2]
			if op2 == "<=" || op2 == ">=" || op2 == "!=" || op2 == "<>" {
				list.tokens[list.count] = Token{kind = .Symbol, text = op2}
				list.count += 1
				pos += 2
				continue
			}
		}

		if strings.index_byte("=<>(),*", input[pos]) != -1 {
			list.tokens[list.count] = Token{kind = .Symbol, text = input[pos:pos + 1]}
			list.count += 1
			pos += 1
			continue
		}

		if is_digit(input[pos]) {
			start := pos
			for pos < length && is_digit(input[pos]) && pos - start < MAX_STR_LEN - 1 {
				pos += 1
			}
			list.tokens[list.count] = Token{kind = .Number, text = input[start:pos]}
			list.count += 1
			continue
		}

		if is_alpha(input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\' {
			start := pos
			for pos < length &&
			    (is_alnum(input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') &&
			    pos - start < MAX_STR_LEN - 1 {
				pos += 1
			}
			list.tokens[list.count] = Token{kind = .Identifier, text = input[start:pos]}
			list.count += 1
			continue
		}

		pos += 1
	}

	list.tokens[list.count] = Token{kind = .End, text = ""}
}

peek_token :: proc(list: ^Token_List) -> ^Token {
	return &list.tokens[list.cursor]
}

advance_token :: proc(list: ^Token_List) -> ^Token {
	t := &list.tokens[list.cursor]
	list.cursor += 1
	return t
}

token_text :: proc(t: ^Token) -> string {
	return t.text
}

match_token :: proc(list: ^Token_List, text: string) -> bool {
	if strings.equal_fold(token_text(peek_token(list)), text) {
		list.cursor += 1
		return true
	}
	return false
}

parse_full_int :: proc(s: string) -> (i64, bool) {
	n: int
	v, ok := strconv.parse_i64_of_base(s, 10, &n)
	if !ok || n != len(s) do return 0, false
	return v, true
}

// Parses a single comparison: <column> <op> <value>
parse_comparison :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
	col := advance_token(list)
	op := advance_token(list)
	val := advance_token(list)

	if col.kind != .Identifier do return .Syntax_Error

	col_idx := schema_find_column(schema, token_text(col))
	if col_idx < 0 do return .Syntax_Error

	cmp := new(Expr)
	cmp.type = .Comparison
	buf_set(cmp.column[:], buf_str(schema.columns[col_idx].name[:]))

	op_text := token_text(op)
	if op_text == "=" {
		cmp.op = .Eq
	} else if op_text == "!=" || op_text == "<>" {
		cmp.op = .Neq
	} else if op_text == ">=" {
		cmp.op = .Ge
	} else if op_text == "<=" {
		cmp.op = .Le
	} else if op_text == ">" {
		cmp.op = .Gt
	} else if op_text == "<" {
		cmp.op = .Lt
	} else {
		free(cmp)
		return .Syntax_Error
	}

	if schema.columns[col_idx].type == .Int {
		cmp.value.is_string = false
		int_v, ok := parse_full_int(token_text(val))
		if !ok {
			free(cmp)
			return .Syntax_Error
		}
		if schema.columns[col_idx].is_primary_key && int_v < 0 {
			free(cmp)
			return .Negative_Id
		}
		cmp.value.int_value = u32(int_v)
	} else {
		cmp.value.is_string = true
		buf_set(cmp.value.str_value[:], token_text(val))
	}

	out^ = cmp
	return .Success
}

parse_expr :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
	return parse_or(list, schema, out)
}

parse_primary :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
	if peek_token(list).kind == .Symbol && token_text(peek_token(list)) == "(" {
		advance_token(list) // consume '('
		res := parse_expr(list, schema, out)
		if res != .Success do return res
		if peek_token(list).kind != .Symbol || token_text(peek_token(list)) != ")" {
			free_expr(out^)
			out^ = nil
			return .Syntax_Error
		}
		advance_token(list) // consume ')'
		return .Success
	}
	return parse_comparison(list, schema, out)
}

parse_and :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
	left: ^Expr = nil
	res := parse_primary(list, schema, &left)
	if res != .Success do return res

	for match_token(list, "AND") {
		right: ^Expr = nil
		res = parse_primary(list, schema, &right)
		if res != .Success {
			free_expr(left)
			return res
		}
		parent := new(Expr)
		parent.type = .Logical
		parent.log_op = .And
		parent.left = left
		parent.right = right
		left = parent
	}
	out^ = left
	return .Success
}

parse_or :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
	left: ^Expr = nil
	res := parse_and(list, schema, &left)
	if res != .Success do return res

	for match_token(list, "OR") {
		right: ^Expr = nil
		res = parse_and(list, schema, &right)
		if res != .Success {
			free_expr(left)
			return res
		}
		parent := new(Expr)
		parent.type = .Logical
		parent.log_op = .Or
		parent.left = left
		parent.right = right
		left = parent
	}
	out^ = left
	return .Success
}

parse_where_clause :: proc(list: ^Token_List, schema: ^Schema, statement: ^Statement) -> Prepare_Result {
	if !match_token(list, "WHERE") do return .Success
	return parse_expr(list, schema, &statement.where_clause)
}

prepare_statement :: proc(list: ^Token_List, db: ^Database, statement: ^Statement) -> Prepare_Result {
	statement^ = Statement{}
	if list.count == 0 || peek_token(list).kind == .End {
		return .Syntax_Error
	}

	first := peek_token(list)
	first_text := token_text(first)

	// CREATE TABLE <name> (col1 INT PRIMARY KEY, col2 VARCHAR(32), ...)
	if strings.equal_fold(first_text, "create") {
		advance_token(list)
		if !match_token(list, "table") do return .Syntax_Error
		tbl := advance_token(list)
		statement.type = .Create
		buf_set(statement.table_name[:], token_text(tbl))

		if db_find_table(db, token_text(tbl)) != nil do return .Table_Exists

		if token_text(peek_token(list)) != "(" do return .Syntax_Error
		advance_token(list) // consume '('

		statement.created_schema = Schema{}
		buf_set(statement.created_schema.table_name[:], token_text(tbl))

		for list.cursor < list.count && token_text(peek_token(list)) != ")" {
			col_name := advance_token(list)
			if col_name.kind != .Identifier do return .Syntax_Error

			col_type := advance_token(list)
			dt := Data_Type.Int
			col_len: u32 = 0

			ct := token_text(col_type)
			if strings.equal_fold(ct, "int") || strings.equal_fold(ct, "integer") {
				dt = .Int
			} else if strings.equal_fold(ct, "varchar") || strings.equal_fold(ct, "string") || strings.equal_fold(ct, "char") {
				dt = .Varchar
				col_len = 32
				if token_text(peek_token(list)) == "(" {
					advance_token(list)
					len_tok := advance_token(list)
					lv, _ := strconv.parse_uint(token_text(len_tok), 10)
					col_len = u32(lv)
					if token_text(peek_token(list)) == ")" do advance_token(list)
				}
			} else {
				return .Syntax_Error
			}

			is_pk := false
			if strings.equal_fold(token_text(peek_token(list)), "primary") {
				advance_token(list)
				if strings.equal_fold(token_text(peek_token(list)), "key") do advance_token(list)
				is_pk = true
			}

			schema_add_column(&statement.created_schema, token_text(col_name), dt, col_len, is_pk)

			if token_text(peek_token(list)) == "," {
				advance_token(list)
			}
		}

		if token_text(peek_token(list)) == ")" do advance_token(list)
		return .Success
	}

	// DROP TABLE <name>
	if strings.equal_fold(first_text, "drop") {
		advance_token(list)
		if !match_token(list, "table") do return .Syntax_Error
		tbl := advance_token(list)
		statement.type = .Drop
		buf_set(statement.table_name[:], token_text(tbl))

		if db_find_table(db, token_text(tbl)) == nil do return .Table_Not_Found
		return .Success
	}

	// ALTER TABLE <name> ADD [COLUMN] <col> <type>[(len)] [DEFAULT <value>]
	// ALTER TABLE <name> DROP [COLUMN] <col>
	if strings.equal_fold(first_text, "alter") {
		advance_token(list)
		if !match_token(list, "table") do return .Syntax_Error
		tbl := advance_token(list)
		buf_set(statement.table_name[:], token_text(tbl))

		t := db_find_table(db, token_text(tbl))
		if t == nil do return .Table_Not_Found
		statement.resolved_table = t

		if match_token(list, "add") {
			match_token(list, "column")
			col_tok := advance_token(list)
			if col_tok.kind != .Identifier do return .Syntax_Error
			buf_set(statement.alter_column_name[:], token_text(col_tok))
			if t.schema.num_columns >= MAX_COLUMNS do return .Too_Many_Columns
			if schema_find_column(&t.schema, token_text(col_tok)) >= 0 do return .Column_Exists

			type_tok := advance_token(list)
			ct := token_text(type_tok)
			dt := Data_Type.Int
			col_len: u32 = 0
			if strings.equal_fold(ct, "int") || strings.equal_fold(ct, "integer") {
				dt = .Int
			} else if strings.equal_fold(ct, "varchar") || strings.equal_fold(ct, "string") || strings.equal_fold(ct, "char") {
				dt = .Varchar
				col_len = 32
				if token_text(peek_token(list)) == "(" {
					advance_token(list)
					len_tok := advance_token(list)
					lv, _ := strconv.parse_uint(token_text(len_tok), 10)
					col_len = u32(lv)
					if token_text(peek_token(list)) == ")" do advance_token(list)
				}
			} else {
				return .Syntax_Error
			}
			statement.alter_column_type = dt
			statement.alter_column_length = col_len
			statement.alter_default.type = dt // zero-value default (0 or "") unless overridden below

			if match_token(list, "default") {
				val_tok := advance_token(list)
				if dt == .Int {
					iv, ok := parse_full_int(token_text(val_tok))
					if !ok do return .Syntax_Error
					statement.alter_default.int_val = i32(iv)
				} else {
					vt := token_text(val_tok)
					if len(vt) > int(col_len) do return .String_Too_Long
					buf_set(statement.alter_default.str_val[:], vt)
				}
			}

			statement.type = .Alter_Add_Column
			return .Success
		}

		if match_token(list, "drop") {
			match_token(list, "column")
			col_tok := advance_token(list)
			if col_tok.kind != .Identifier do return .Syntax_Error
			buf_set(statement.alter_column_name[:], token_text(col_tok))

			idx := schema_find_column(&t.schema, token_text(col_tok))
			if idx < 0 do return .Column_Not_Found
			if t.schema.columns[idx].is_primary_key do return .Cannot_Drop_Primary_Key
			if t.schema.num_columns <= 1 do return .Cannot_Drop_Last_Column

			statement.type = .Alter_Drop_Column
			return .Success
		}

		return .Syntax_Error
	}

	if strings.equal_fold(first_text, "begin") || strings.equal_fold(first_text, "start") {
		advance_token(list)
		if strings.equal_fold(first_text, "start") do match_token(list, "transaction")
		statement.type = .Begin
		return .Success
	}

	if strings.equal_fold(first_text, "commit") {
		advance_token(list)
		statement.type = .Commit
		return .Success
	}

	if strings.equal_fold(first_text, "rollback") {
		advance_token(list)
		statement.type = .Rollback
		return .Success
	}

	// INSERT INTO <table> VALUES (val1, val2, ...)
	if strings.equal_fold(first_text, "insert") {
		advance_token(list)
		match_token(list, "into")
		tbl := advance_token(list)
		buf_set(statement.table_name[:], token_text(tbl))

		t := db_find_table(db, token_text(tbl))
		if t == nil do return .Table_Not_Found
		schema := &t.schema

		if token_text(peek_token(list)) == "(" {
			for list.cursor < list.count && token_text(peek_token(list)) != ")" do advance_token(list)
			if token_text(peek_token(list)) == ")" do advance_token(list)
		}

		if !match_token(list, "values") do return .Syntax_Error
		if token_text(peek_token(list)) == "(" do advance_token(list)

		statement.type = .Insert
		statement.resolved_table = t
		statement.row_to_insert.num_values = schema.num_columns

		for i in 0 ..< schema.num_columns {
			val_tok := advance_token(list)
			if token_text(peek_token(list)) == "," do advance_token(list)

			v := &statement.row_to_insert.values[i]
			v.type = schema.columns[i].type

			if schema.columns[i].type == .Int {
				int_v, ok := parse_full_int(token_text(val_tok))
				if !ok do return .Syntax_Error
				if schema.columns[i].is_primary_key && int_v < 0 do return .Negative_Id
				v.int_val = i32(int_v)
			} else {
				vt := token_text(val_tok)
				if len(vt) > int(schema.columns[i].length) do return .String_Too_Long
				buf_set(v.str_val[:], vt)
			}
		}

		if token_text(peek_token(list)) == ")" do advance_token(list)
		return .Success
	}

	// SELECT [* | COUNT(*)] FROM <table> [WHERE <expr>]
	if strings.equal_fold(first_text, "select") {
		advance_token(list)
		statement.type = .Select

		if strings.equal_fold(token_text(peek_token(list)), "count") {
			advance_token(list)
			if token_text(peek_token(list)) == "(" do advance_token(list)
			if token_text(peek_token(list)) == "*" do advance_token(list)
			if token_text(peek_token(list)) == ")" do advance_token(list)
			statement.is_count = true
		} else if token_text(peek_token(list)) == "*" {
			advance_token(list)
		}

		if !match_token(list, "from") do return .Syntax_Error
		tbl := advance_token(list)
		buf_set(statement.table_name[:], token_text(tbl))

		t := db_find_table(db, token_text(tbl))
		if t == nil do return .Table_Not_Found
		statement.resolved_table = t

		res := parse_where_clause(list, &t.schema, statement)
		if res != .Success do return res

		// ORDER BY <col> [ASC|DESC]
		if match_token(list, "order") {
			if !match_token(list, "by") do return .Syntax_Error
			col_tok := advance_token(list)
			if col_tok.kind != .Identifier do return .Syntax_Error
			buf_set(statement.order_by_column[:], token_text(col_tok))

			if schema_find_column(&t.schema, token_text(col_tok)) < 0 do return .Order_By_Column_Not_Found
			statement.has_order_by = true

			if match_token(list, "desc") {
				statement.order_by_desc = true
			} else {
				match_token(list, "asc")
			}
		}

		// LIMIT <n> [OFFSET <n>]
		if match_token(list, "limit") {
			n_tok := advance_token(list)
			lv, ok := parse_full_int(token_text(n_tok))
			if !ok || lv < 0 do return .Syntax_Error
			statement.limit = u32(lv)
			statement.has_limit = true

			if match_token(list, "offset") {
				o_tok := advance_token(list)
				ov, ok2 := parse_full_int(token_text(o_tok))
				if !ok2 || ov < 0 do return .Syntax_Error
				statement.offset = u32(ov)
			}
		}

		return .Success
	}

	// UPDATE <table> SET col1 = val1 [, col2 = val2] [WHERE <expr>]
	if strings.equal_fold(first_text, "update") {
		advance_token(list)
		statement.type = .Update
		statement.is_set_update = true

		tbl := advance_token(list)
		buf_set(statement.table_name[:], token_text(tbl))

		t := db_find_table(db, token_text(tbl))
		if t == nil do return .Table_Not_Found
		statement.resolved_table = t

		match_token(list, "set")

		for list.cursor < list.count &&
		    !strings.equal_fold(token_text(peek_token(list)), "where") &&
		    token_text(peek_token(list)) != ";" {
			if token_text(peek_token(list)) == "," {
				advance_token(list)
				continue
			}

			col := advance_token(list)
			if !match_token(list, "=") do return .Syntax_Error
			val := advance_token(list)

			if statement.num_update_assignments < MAX_ASSIGNMENTS {
				assign := &statement.update_assignments[statement.num_update_assignments]
				statement.num_update_assignments += 1
				buf_set(assign.column_name[:], token_text(col))
				buf_set(assign.value_text[:], token_text(val))
			}
		}

		return parse_where_clause(list, &t.schema, statement)
	}

	// DELETE FROM <table> [WHERE <expr>]
	if strings.equal_fold(first_text, "delete") {
		advance_token(list)
		statement.type = .Delete

		if !match_token(list, "from") do return .Syntax_Error
		tbl := advance_token(list)
		buf_set(statement.table_name[:], token_text(tbl))

		t := db_find_table(db, token_text(tbl))
		if t == nil do return .Table_Not_Found
		statement.resolved_table = t

		return parse_where_clause(list, &t.schema, statement)
	}

	return .Unrecognized_Statement
}


// ============================================================================
// Execution Engine
// ============================================================================

row_matches_where :: proc(row: ^Dynamic_Row, statement: ^Statement, schema: ^Schema) -> bool {
	if statement.where_clause == nil do return true
	return evaluate_expr(statement.where_clause, row, schema)
}

execute_insert :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
	if table.pager.num_pages + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES {
		return .Table_Full
	}

	key_to_insert := get_pk_value(&statement.row_to_insert, &table.schema)
	cursor := table_find(table, key_to_insert)

	node := pager_get_page(table.pager, cursor.page_num)
	num_cells := leaf_node_num_cells(node)^
	if cursor.cell_num < num_cells {
		key_at_index := leaf_node_key(node, cursor.cell_num, table.schema.row_size)^
		if key_at_index == key_to_insert {
			free(cursor)
			return .Duplicate_Key
		}
	}

	leaf_node_insert(cursor, key_to_insert, &statement.row_to_insert)
	free(cursor)
	return .Success
}

Pending_Update :: struct {
	old_key: u32,
	new_key: u32,
	row:     Dynamic_Row,
}

execute_update :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
	pending := make([dynamic]Pending_Update, 0, 16)
	defer delete(pending)

	cursor := table_start(table)
	row: Dynamic_Row

	for !cursor.end_of_table {
		deserialize_row(cursor_value(cursor), &row, &table.schema)
		if row_matches_where(&row, statement, &table.schema) {
			old_key := get_pk_value(&row, &table.schema)

			for i in 0 ..< statement.num_update_assignments {
				assign := &statement.update_assignments[i]
				col_idx := schema_find_column(&table.schema, buf_str(assign.column_name[:]))
				if col_idx >= 0 && u32(col_idx) < row.num_values {
					if table.schema.columns[col_idx].type == .Int {
						iv, _ := parse_full_int(buf_str(assign.value_text[:]))
						row.values[col_idx].int_val = i32(iv)
					} else {
						buf_set(row.values[col_idx].str_val[:], buf_str(assign.value_text[:]))
					}
				}
			}

			append(&pending, Pending_Update{old_key = old_key, new_key = get_pk_value(&row, &table.schema), row = row})
		}
		cursor_advance(cursor)
	}
	free(cursor)

	applied: u32 = 0
	skipped_duplicates: u32 = 0

	for i in 0 ..< len(pending) {
		u := &pending[i]

		if u.new_key == u.old_key {
			c := table_find(table, u.old_key)
			node := pager_get_page(table.pager, c.page_num)
			num_cells := leaf_node_num_cells(node)^
			if c.cell_num < num_cells && leaf_node_key(node, c.cell_num, table.schema.row_size)^ == u.old_key {
				serialize_row(&u.row, cursor_value(c), &table.schema)
				applied += 1
			}
			free(c)
			continue
		}

		dest := table_find(table, u.new_key)
		dest_node := pager_get_page(table.pager, dest.page_num)
		dest_num_cells := leaf_node_num_cells(dest_node)^
		duplicate := dest.cell_num < dest_num_cells && leaf_node_key(dest_node, dest.cell_num, table.schema.row_size)^ == u.new_key
		free(dest)

		if duplicate {
			skipped_duplicates += 1
			continue
		}

		old_cursor := table_find(table, u.old_key)
		old_node := pager_get_page(table.pager, old_cursor.page_num)
		old_num_cells := leaf_node_num_cells(old_node)^
		if old_cursor.cell_num < old_num_cells && leaf_node_key(old_node, old_cursor.cell_num, table.schema.row_size)^ == u.old_key {
			leaf_node_delete(old_cursor)
			free(old_cursor)

			insert_cursor := table_find(table, u.new_key)
			leaf_node_insert(insert_cursor, u.new_key, &u.row)
			free(insert_cursor)
			applied += 1
		} else {
			free(old_cursor)
		}
	}

	if skipped_duplicates > 0 {
		fmt.printf("UPDATE %d (skipped %d due to duplicate key)\n", applied, skipped_duplicates)
	} else {
		fmt.printf("UPDATE %d\n", applied)
	}
	return .Success
}

execute_delete :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
	keys_to_delete := make([dynamic]u32, 0, 64)
	defer delete(keys_to_delete)

	cursor := table_start(table)
	row: Dynamic_Row

	for !cursor.end_of_table {
		deserialize_row(cursor_value(cursor), &row, &table.schema)
		if row_matches_where(&row, statement, &table.schema) {
			append(&keys_to_delete, get_pk_value(&row, &table.schema))
		}
		cursor_advance(cursor)
	}
	free(cursor)

	for key in keys_to_delete {
		c := table_find(table, key)
		node := pager_get_page(table.pager, c.page_num)
		num_cells := leaf_node_num_cells(node)^
		if c.cell_num < num_cells && leaf_node_key(node, c.cell_num, table.schema.row_size)^ == key {
			leaf_node_delete(c)
		}
		free(c)
	}
	fmt.printf("DELETE %d\n", len(keys_to_delete))
	return .Success
}

Order_By_Ctx :: struct {
	col_idx:  u32,
	col_type: Data_Type,
	desc:     bool,
}

order_by_less :: proc(a, b: Dynamic_Row, user_data: rawptr) -> bool {
	aa := a
	bb := b
	ctx := cast(^Order_By_Ctx)user_data
	cmp := 0
	if ctx.col_type == .Int {
		av := aa.values[ctx.col_idx].int_val
		bv := bb.values[ctx.col_idx].int_val
		if av < bv {
			cmp = -1
		} else if av > bv {
			cmp = 1
		}
	} else {
		cmp = strings.compare(buf_str(aa.values[ctx.col_idx].str_val[:]), buf_str(bb.values[ctx.col_idx].str_val[:]))
	}
	if ctx.desc do return cmp > 0
	return cmp < 0
}

execute_select :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
	// COUNT(*) ignores ORDER BY / LIMIT -- it always reports the total
	// number of matching rows.
	if statement.is_count {
		cursor := table_start(table)
		row: Dynamic_Row
		match_count: u32 = 0
		for !cursor.end_of_table {
			deserialize_row(cursor_value(cursor), &row, &table.schema)
			if row_matches_where(&row, statement, &table.schema) do match_count += 1
			cursor_advance(cursor)
		}
		free(cursor)
		fmt.printf("%d row(s).\n", match_count)
		return .Success
	}

	matches := make([dynamic]Dynamic_Row, 0, 64)
	defer delete(matches)

	cursor := table_start(table)
	row: Dynamic_Row
	for !cursor.end_of_table {
		deserialize_row(cursor_value(cursor), &row, &table.schema)
		if row_matches_where(&row, statement, &table.schema) {
			append(&matches, row)
		}
		cursor_advance(cursor)
	}
	free(cursor)

	if statement.has_order_by {
		col_idx := schema_find_column(&table.schema, buf_str(statement.order_by_column[:]))
		if col_idx >= 0 {
			ctx := Order_By_Ctx{
				col_idx  = u32(col_idx),
				col_type = table.schema.columns[col_idx].type,
				desc     = statement.order_by_desc,
			}
			slice.sort_by_with_data(matches[:], order_by_less, &ctx)
		}
	}

	total := u32(len(matches))
	start_i := statement.offset
	if start_i > total do start_i = total
	end_i := total
	if statement.has_limit {
		capped := start_i + statement.limit
		if capped < end_i do end_i = capped
	}

	for i in start_i ..< end_i {
		r := matches[i]
		print_row(&r, &table.schema)
	}

	return .Success
}

execute_begin :: proc(db: ^Database) -> Execute_Result {
	if db.in_transaction do return .Tx_Already_Active

	db.in_transaction = true
	db.tx_original_num_pages = db.pager.num_pages

	for i in 0 ..< TABLE_MAX_PAGES {
		page_ptr := db.pager.pages[i]
		if page_ptr != nil {
			db.tx_backup[i], _ = mem.alloc(PAGE_SIZE)
			mem.copy(db.tx_backup[i], page_ptr, PAGE_SIZE)
			db.tx_was_cached[i] = true
		} else {
			db.tx_backup[i] = nil
			db.tx_was_cached[i] = false
		}
	}
	return .Success
}

execute_commit :: proc(db: ^Database) -> Execute_Result {
	if !db.in_transaction do return .No_Active_Tx

	tx_free_backups(db)
	for i in 0 ..< db.pager.num_pages {
		if db.pager.pages[i] != nil {
			pager_flush(db.pager, i)
		}
	}
	db.in_transaction = false
	return .Success
}

execute_rollback :: proc(db: ^Database) -> Execute_Result {
	if !db.in_transaction do return .No_Active_Tx

	for i in 0 ..< TABLE_MAX_PAGES {
		if u32(i) < db.tx_original_num_pages {
			if db.tx_was_cached[i] {
				mem.copy(db.pager.pages[i], db.tx_backup[i], PAGE_SIZE)
				mem.free(db.tx_backup[i])
				db.tx_backup[i] = nil
			} else if db.pager.pages[i] != nil {
				mem.free(db.pager.pages[i])
				db.pager.pages[i] = nil
			}
		} else {
			if db.pager.pages[i] != nil {
				mem.free(db.pager.pages[i])
				db.pager.pages[i] = nil
			}
			if db.tx_backup[i] != nil {
				mem.free(db.tx_backup[i])
				db.tx_backup[i] = nil
			}
		}
	}
	db.pager.num_pages = db.tx_original_num_pages
	db.in_transaction = false

	// Any CREATE TABLE done inside the rolled-back transaction must also be
	// forgotten from the in-memory catalog view.
	db_reload_catalog(db)
	return .Success
}

execute_statement :: proc(statement: ^Statement, db: ^Database) -> Execute_Result {
	switch statement.type {
	case .Insert:
		return execute_insert(statement, statement.resolved_table)
	case .Select:
		return execute_select(statement, statement.resolved_table)
	case .Update:
		return execute_update(statement, statement.resolved_table)
	case .Delete:
		return execute_delete(statement, statement.resolved_table)
	case .Create:
		if !db_create_table(db, &statement.created_schema) {
			return .Catalog_Full
		}
		fmt.printf(
			"CREATE TABLE %s (%d columns configured)\n",
			buf_str(statement.created_schema.table_name[:]),
			statement.created_schema.num_columns,
		)
		return .Success
	case .Drop:
		if !db_drop_table(db, buf_str(statement.table_name[:])) {
			return .Not_Found
		}
		fmt.printf("DROP TABLE %s\n", buf_str(statement.table_name[:]))
		return .Success
	case .Alter_Add_Column:
		if !db_alter_add_column(
			db,
			statement.resolved_table,
			buf_str(statement.alter_column_name[:]),
			statement.alter_column_type,
			statement.alter_column_length,
			statement.alter_default,
		) {
			return .Not_Found
		}
		fmt.printf("ALTER TABLE %s ADD COLUMN %s\n", buf_str(statement.table_name[:]), buf_str(statement.alter_column_name[:]))
		return .Success
	case .Alter_Drop_Column:
		if !db_alter_drop_column(db, statement.resolved_table, buf_str(statement.alter_column_name[:])) {
			return .Not_Found
		}
		fmt.printf("ALTER TABLE %s DROP COLUMN %s\n", buf_str(statement.table_name[:]), buf_str(statement.alter_column_name[:]))
		return .Success
	case .Begin:
		return execute_begin(db)
	case .Commit:
		return execute_commit(db)
	case .Rollback:
		return execute_rollback(db)
	}
	return .Success
}

// ============================================================================
// Shell & Meta Commands
// ============================================================================

print_help :: proc() {
	fmt.print(
		"SQL Commands:\n" +
		"  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), <col3> INT, ...);\n" +
		"  DROP TABLE <name>;\n" +
		"  ALTER TABLE <name> ADD [COLUMN] <col> <type>[(len)] [DEFAULT <value>];\n" +
		"  ALTER TABLE <name> DROP [COLUMN] <col>;\n" +
		"  INSERT INTO <name> VALUES (<val1>, '<val2>', ...);\n" +
		"  SELECT * FROM <name> [WHERE <expr>] [ORDER BY <col> [ASC|DESC]] [LIMIT <n> [OFFSET <n>]];\n" +
		"  SELECT COUNT(*) FROM <name> [WHERE <expr>];\n" +
		"  UPDATE <name> SET <col> = <val> WHERE <expr>;\n" +
		"  DELETE FROM <name> WHERE <expr>;\n" +
		"  (WHERE supports =, !=, <>, <, >, <=, >=, AND, OR, and parentheses ())\n" +
		"  BEGIN; | COMMIT; | ROLLBACK;\n" +
		"  A single database file may hold multiple tables.\n" +
		"Meta commands:\n" +
		"  \\q or .exit                quit the shell\n" +
		"  \\dt or .tables              list tables in this file\n" +
		"  \\d [name] or .btree [name]  print a table's B+tree structure\n" +
		"  \\c [name] or .constants     print page size constants for a table\n" +
		"  \\? or .help                 show this message\n",
	)
}

print_constants :: proc(table: ^Table) {
	fmt.printf(
		"ROW_SIZE: %d\n" +
		"COMMON_NODE_HEADER_SIZE: %d\n" +
		"LEAF_NODE_HEADER_SIZE: %d\n" +
		"LEAF_NODE_CELL_SIZE: %d\n" +
		"LEAF_NODE_MAX_CELLS: %d\n",
		table.schema.row_size,
		int(COMMON_NODE_HEADER_SIZE),
		int(LEAF_NODE_HEADER_SIZE),
		leaf_node_cell_size(table.schema.row_size),
		leaf_node_max_cells(table.schema.row_size),
	)
}

// Resolves an optional table-name argument for meta commands like ".btree"
// and ".constants". If no name was given, this only succeeds when the
// database has exactly one table (so the command stays unambiguous).
meta_resolve_table :: proc(db: ^Database, arg: string) -> (^Table, bool) {
	if arg != "" {
		t := db_find_table(db, arg)
		if t == nil {
			fmt.printf("No such table '%s'.\n", arg)
			return nil, false
		}
		return t, true
	}
	if db.num_tables == 1 {
		return &db.tables[0], true
	}
	if db.num_tables == 0 {
		fmt.println("This database has no tables yet.")
	} else {
		fmt.println("This database has multiple tables; specify one, e.g. '.btree users'.")
	}
	return nil, false
}

do_meta_command :: proc(input: string, db: ^Database) -> Meta_Command_Result {
	parts := strings.split(input, " ")
	defer delete(parts)
	cmd := parts[0]
	arg := ""
	if len(parts) > 1 do arg = strings.trim_space(parts[1])

	if cmd == ".exit" || cmd == "\\q" {
		db_close(db)
		os.exit(0)
	} else if cmd == ".tables" || cmd == "\\dt" {
		if db.num_tables == 0 {
			fmt.println("This database has no tables yet.")
		}
		for i in 0 ..< db.num_tables {
			fmt.printf("  %s (%d columns)\n", buf_str(db.tables[i].schema.table_name[:]), db.tables[i].schema.num_columns)
		}
		return .Success
	} else if cmd == ".btree" || cmd == "\\d" {
		t, ok := meta_resolve_table(db, arg)
		if !ok do return .Success
		fmt.printf("Tree (%s):\n", buf_str(t.schema.table_name[:]))
		print_tree(t, t.root_page_num, 0)
		return .Success
	} else if cmd == ".constants" || cmd == "\\c" {
		t, ok := meta_resolve_table(db, arg)
		if !ok do return .Success
		fmt.printf("Constants (%s):\n", buf_str(t.schema.table_name[:]))
		print_constants(t)
		return .Success
	} else if cmd == ".help" || cmd == "\\?" {
		print_help()
		return .Success
	}
	return .Unrecognized_Command
}

main :: proc() {
	if len(os.args) < 2 {
		fmt.println("Must supply a database filename.")
		os.exit(1)
	}

	filename := os.args[1]
	db := db_open(filename)

	stdin_stream := os.to_stream(os.stdin)
	reader: bufio.Reader
	bufio.reader_init(&reader, stdin_stream)
	defer bufio.reader_destroy(&reader)

	for {
		fmt.print("db=# ")

		line, err := bufio.reader_read_string(&reader, '\n')
		if err != nil {
			break
		}
		defer delete(line)

		input_buffer := strings.trim_right(line, "\r\n")

		if len(input_buffer) == 0 do continue

		if input_buffer[0] == '.' || input_buffer[0] == '\\' {
			switch do_meta_command(input_buffer, db) {
			case .Success:
				continue
			case .Unrecognized_Command:
				fmt.printf("Unrecognized command '%s'\n", input_buffer)
				continue
			}
		}

		tokens: Token_List
		tokenize_input(input_buffer, &tokens)

		statement: Statement
		prep_res := prepare_statement(&tokens, db, &statement)

		switch prep_res {
		case .Success:
			break
		case .Negative_Id:
			fmt.println("ID must be positive.")
			free_expr(statement.where_clause)
			continue
		case .String_Too_Long:
			fmt.println("String is too long for column budget.")
			free_expr(statement.where_clause)
			continue
		case .Syntax_Error:
			fmt.println("Syntax error. Could not parse statement.")
			free_expr(statement.where_clause)
			continue
		case .Unrecognized_Statement:
			fmt.printf("Unrecognized keyword at start of '%s'.\n", input_buffer)
			free_expr(statement.where_clause)
			continue
		case .Table_Not_Found:
			fmt.printf("Error: table '%s' does not exist.\n", buf_str(statement.table_name[:]))
			free_expr(statement.where_clause)
			continue
		case .Table_Exists:
			fmt.printf("Error: table '%s' already exists.\n", buf_str(statement.table_name[:]))
			free_expr(statement.where_clause)
			continue
		case .Column_Exists:
			fmt.printf("Error: column '%s' already exists on table '%s'.\n", buf_str(statement.alter_column_name[:]), buf_str(statement.table_name[:]))
			free_expr(statement.where_clause)
			continue
		case .Column_Not_Found:
			fmt.printf("Error: no such column '%s' on table '%s'.\n", buf_str(statement.alter_column_name[:]), buf_str(statement.table_name[:]))
			free_expr(statement.where_clause)
			continue
		case .Cannot_Drop_Primary_Key:
			fmt.println("Error: cannot drop the primary key column.")
			free_expr(statement.where_clause)
			continue
		case .Cannot_Drop_Last_Column:
			fmt.println("Error: cannot drop a table's only remaining column.")
			free_expr(statement.where_clause)
			continue
		case .Too_Many_Columns:
			fmt.printf("Error: table '%s' already has the maximum of %d columns.\n", buf_str(statement.table_name[:]), MAX_COLUMNS)
			free_expr(statement.where_clause)
			continue
		case .Order_By_Column_Not_Found:
			fmt.printf("Error: no such column '%s' to ORDER BY on table '%s'.\n", buf_str(statement.order_by_column[:]), buf_str(statement.table_name[:]))
			free_expr(statement.where_clause)
			continue
		}

		exec_res := execute_statement(&statement, db)
		free_expr(statement.where_clause)

		switch exec_res {
		case .Success:
			if statement.type == .Insert {
				fmt.println("INSERT 0 1")
			} else if statement.type == .Begin {
				fmt.println("BEGIN")
			} else if statement.type == .Commit {
				fmt.println("COMMIT")
			} else if statement.type == .Rollback {
				fmt.println("ROLLBACK")
			}
		case .Duplicate_Key:
			fmt.println("Error: Duplicate key.")
		case .Not_Found:
			fmt.println("Error: row not found.")
		case .Tx_Already_Active:
			fmt.println("Error: a transaction is already active.")
		case .No_Active_Tx:
			fmt.println("Error: no active transaction.")
		case .Table_Full:
			fmt.printf("Error: table is full (max %d pages).\n", TABLE_MAX_PAGES)
		case .Catalog_Full:
			fmt.printf("Error: this database already has the maximum of %d tables.\n", MAX_TABLES)
		}
	}

	db_close(db)
}
