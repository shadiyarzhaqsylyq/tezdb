package db

import "core:bufio"
import "core:fmt"
import "core:os"
import "core:strings"

main :: proc() {
	if len(os.args) < 2 {
		fmt.println("Usage: db <database_file>")
		os.exit(1)
	}

	db_filename := os.args[1]
	fmt.printf("Opened database: %s\n", db_filename)

	reader: bufio.Reader
	in_stream := os.to_stream(os.stdin)
	bufio.reader_init(&reader, in_stream)
	defer bufio.reader_destroy(&reader)

	for {
		fmt.print("db> ")
		os.flush(os.stdout) // Guarantees prompt displays immediately

		line, err := bufio.reader_read_string(&reader, '\n')
		if err != nil do break
		defer delete(line) // Frees the allocated line memory each loop iteration

		command := strings.trim_space(line)

		if command == "exit" || command == ".exit" {
			fmt.println("Bye!")
			break
		}

		if len(command) == 0 do continue

		// Pass 'command' to your SQL parser / executor here
		fmt.println("Unrecognized command:", command)
	}
}
