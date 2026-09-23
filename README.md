## Educational database in C.





1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY

Types


VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER


VARCHAR, CHAR - default 32


Operators - >=, <=, =, <, >, !=, <>

!= and <> have the same meaning "not equal to".
```
*CREATE*
CREATE TABLE table (a INT PRIMARY KEY, b VARCHAR, c VARCHAR, d VARCHAR, f INT, g VARCHAR);

### Examples for CREATE
CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);
CREATE TABLE movies (id INT PRIMARY KEY, title VARCHAR, isbn VARCHAR, genre VARCHAR, price INT, author VARCHAR);


*INSERT*
INSERT INTO table VALUES (PK num, 'char', 'char', 'char', int num, 'char');

### Examples for INSERT
INSERT INTO emp VALUES (1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY');
INSERT INTO emp VALUES (2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA');
INSERT INTO emp VALUES (3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF');
INSERT INTO emp VALUES (4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA');
INSERT INTO emp VALUES (5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF');
INSERT INTO emp VALUES (6, 'Frank', '3030C-3001b', 'Sales', 11000, 'NY');


INSERT INTO movies VALUES (1, 'The Godfather', '978-0743273565', 'Drama', 15, 'Francis Ford Coppola');
INSERT INTO movies VALUES (2, 'Star Wars 4: A New Hope', '978-0061120084', 'Science Fiction', 18, 'George Lucas');
INSERT INTO movies VALUES (3, 'The Godfather 2', '978-0451524935', 'Drama', 12, 'Francis Ford Coppola');
INSERT INTO movies VALUES (4, 'Dune 2', '978-1449373320', 'Sci-Fi', 45, 'Denis Villeneuve');
INSERT INTO movies VALUES (5, 'Star Wars 6: Return of Jedi', '978-0132350884', 'Science Fiction', 40, 'George Lucas');
INSERT INTO movies VALUES (6, 'Dune', '978-0441172719', 'Sci-Fi', 22, 'Denis Villeneuve');



*SELECT*
SELECT * FROM table;
SELECT col1, col2 FROM emp;
SELECT * FROM table WHERE a = '';
SELECT * FROM table WHERE a = '' AND b > '';
SELECT * FROM table WHERE a = '' OR b = '';
SELECT * FROM table WHERE (a = '' AND b = '') OR c < '';
SELECT * FROM table WHERE a = '' AND (b = '' OR d = '');
SELECT * FROM users WHERE ((id = 1 OR id = 2) AND age > 21) OR (role = 'superuser');


### Examples for SELECT
SELECT * FROM emp;
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY')
(2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA')
(3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF')
(4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA')
(5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF')
(6, 'Frank', '3030C-3001b', 'HR', 11000, 'NY')


SELECT * FROM emp WHERE id = 1;
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY')

SELECT * FROM emp WHERE city = 'NY' AND salary > 10000;
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY')
(6, 'Frank', '3030C-3001b', 'HR', 11000, 'NY')

SELECT * FROM emp WHERE dep = 'HR' OR city = 'LA';
(2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA')
(3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF')
(4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA')
(5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF')
(6, 'Frank', '3030C-3001b', 'HR', 11000, 'NY')

SELECT * FROM emp WHERE (city = 'SF' AND dep = 'HR') OR id < 3;
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY')
(2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA')
(3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF')
(5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF')

SELECT * FROM emp WHERE dep = 'HR' AND (city = 'SF' OR d = 'NY');
(5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF')
(6, 'Frank', '3030C-3001b', 'HR', 11000, 'NY')


*UPDATE*
UPDATE table SET a = '' WHERE b = '';
UPDATE table SET a = '', b = '' WHERE c >= '';
UPDATE table SET a = '' WHERE b <= '' AND (c = '' OR d = '');

### Examples for UPDATE
UPDATE emp SET a = 'SF' WHERE id = 1;
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'SF')
(2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA')
(3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF')
(4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA')
(5, 'Evan', 'XXX', 'HR', 3500, 'Miami')
(6, 'Frank', 'XXX', 'HR', 11000, 'Miami')


UPDATE emp SET did = 'XXX', city = 'Miami' WHERE id >= 5;
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'SF')
(2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA')
(3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF')
(4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA')
(5, 'Evan', 'XXX', 'HR', 3500, 'Miami')
(6, 'Frank', 'XXX', 'HR', 11000, 'Miami')

UPDATE emp SET salary = 1 WHERE id <= 5 AND (city = 'LA' OR city = 'SF');
(1, 'Alice', '1010A-1001a', 'Engineering', 120000, 'NY')
(2, 'Bob', '1010A-1001b', 'IT', 1, 'LA')
(3, 'Charlie', '2020B-2001a', 'HR', 1, 'SF')
(4, 'Diana', '2020B-2001b', 'Finance', 1, 'LA')
(5, 'Evan', 'XXX', 'HR', 3500, 'Miami')
(6, 'Frank', 'XXX', 'HR', 11000, 'Miami')


*DELETE*
DELETE FROM table WHERE a > '';
DELETE FROM table WHERE a < '';


DELETE FROM table WHERE a = '' OR b = '';
DELETE FROM table WHERE a = '' AND b = '';


DELETE FROM table WHERE (a = '' AND b = '') OR c <= '';
DELETE FROM table WHERE a > '' AND (b = '' OR c = '');

SELECT COUNT(*) FROM table;
SELECT COUNT(*) FROM table WHERE <expr>;



DROP/ALTER TABLE
DROP TABLE table;
ALTER TABLE table DROP [Column] <col>;
ALTER TABLE table ADD [Column] <col> int|VARCHAR(n);
ALTER TABLE table RENAME TO <new name>;
ALTER TABLE table RENAME COLUMN <old> TO <new>;


SELECT * FROM table ORDER BY <col>; --ORDER BY uses ASC by default. works on VARCHAR too
SELECT * FROM table ORDER BY <col> DESC; --ASC/DESC work without ORDER BY, but with ORDER BY is better.
SELECT * FROM table ORDER BY <col> ASC;  


*LIMIT/OFFSET* 
Composable with ORDER BY, WHERE
SELECT * FROM table ORDER BY id DESC; --returns rows in Descending order
SELECT * FROM table ORDER BY id ASC; --return rows in Ascending order. ORDER BY uses ASC by default

SELECT * FROM table LIMIT 2; --returns first 2 rows
SELECT * FROM table ORDER BY id LIMIT 2; --returns first 2 rows

SELECT * FROM table OFFSET 2;
SELECT * FROM table ORDER BY id OFFSET 2; --skips first 2 rows and returns next rows
SELECT * FROM table ORDER BY id DESC LIMIT 2;
SELECT * FROM table ORDER BY id LIMIT 4 OFFSET 2; --skips first 2 rows returns 4 next rows
SELECT * FROM table ORDER BY id DESC LIMIT 4 OFFSET 2;



SELECT SUM(col) FROM table;
SELECT AVG(col) FROM table;
SELECT MIN(col) FROM table;
SELECT MAX(col) FROM table;



*JOIN*
CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(32));
INSERT INTO dept VALUES (101, 'IT');
INSERT INTO dept VALUES (102, 'Eng');
INSERT INTO dept VALUES (103, 'Finance');


CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(32), dept_id INT, salary INT);
INSERT INTO emp VALUES (1, 'Alice', 101, 12000);
INSERT INTO emp VALUES (2, 'Bob', 102, 9000);
INSERT INTO emp VALUES (3, 'Charlie', 103, 10000);
INSERT INTO emp VALUES (4, 'Diana', 104, 13000);
INSERT INTO emp VALUES (5, 'Evan', 105, 3000);
INSERT INTO emp VALUES (6, 'Frank', 106, 20000);

-- basic inner join
SELECT * FROM emp JOIN dept ON emp.dept_id = dept.id;

-- WHERE, ORDER BY, LIMIT, and aggregates all work on the joined result
SELECT * FROM emp JOIN dept ON emp.dept_id = dept.id WHERE dept.name = 'Eng';
SELECT * FROM emp JOIN dept ON emp.dept_id = dept.id ORDER BY salary DESC LIMIT 1;
SELECT SUM(salary) FROM emp JOIN dept ON emp.dept_id = dept.id WHERE dept.name = 'Eng';

-- unqualified column names work too, as long as they're not ambiguous
SELECT * FROM emp JOIN dept ON emp.dept_id = dept.id WHERE salary > 80000;




gcc -Wall -Wextra db.c -o db

\q - exit,\? - for help

./db db.sql
./db sql
```





Not Implemented - Buffer Pool Manager, WAL/Recovery, Catalog, LRU-K replacer, Disk Scheduler, Disk Manager, query optimizer, executor, No free-page list, database wide transactions, Joins, foreign keys, USE statement


