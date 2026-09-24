## Educational database in C.




Multitable


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY

Types


VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER


VARCHAR, CHAR - default 32


Operators - >=, <=, =, <, >, !=, <>

!= and <> have the same meaning "not equal to".

Column projection, Batch Execution / File Loading
```
*CREATE*
CREATE TABLE table (a INT PRIMARY KEY, b VARCHAR, c VARCHAR, d VARCHAR, f INT, g VARCHAR);


*INSERT*
INSERT INTO table VALUES (PK num, 'char', 'char', 'char', int num, 'char');

*SELECT*
SELECT * FROM table;
SELECT col1, col2 FROM emp;
SELECT * FROM table WHERE a = '';
SELECT * FROM table WHERE a = '' AND b > '';
SELECT * FROM table WHERE a = '' OR b = '';
SELECT * FROM table WHERE (a = '' AND b = '') OR c < '';
SELECT * FROM table WHERE a = '' AND (b = '' OR d = '');
SELECT * FROM users WHERE ((id = 1 OR id = 2) AND age > 21) OR (role = 'superuser');


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

Batch Execution / File Loading
./db sql < init.sql
```





Not Implemented - Buffer Pool Manager, WAL/Recovery, Catalog, LRU-K replacer, Disk Scheduler, Disk Manager, query optimizer, executor, No free-page list, database wide transactions, Joins, foreign keys, USE statement


