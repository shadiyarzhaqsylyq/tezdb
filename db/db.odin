package db

import "core:fmt"
import "core:os"
import "core:bufio"
import "core:strings"

main :: proc () {
    if len(os.args) < 2 {
		fmt.println("Must supply a database filename.")
		os.exit(1)
	}

	filename := os.args[1]
	db := db_open(filename)


    reader: bufio.Reader
    
    in_stream := os.to_stream(os.stdin)
    bufio.reader_init(&reader, in_stream)
    for {
        fmt.print("db> ")
        err := os.flush(os.stdin)
        line, _ := bufio.reader_read_string(&reader, '\n')

		input_buffer := strings.trim_right(line, "\r\n")
        if strings.compare("exit", strings.trim_space(line)) == 0 {
            break
        } else {
            fmt.println("command: ", line)
			fmt.println("inputbuf: ", input_buffer)
			
        }
    }
}
