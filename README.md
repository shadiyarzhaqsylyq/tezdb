## Educational database in C.




Multitable


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY

Types


VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER


VARCHAR, CHAR - default 32


Operators - >=, <=, =, <, >, !=, <>

!= and <> have the same meaning "not equal to".

Column projection, Batch Execution / File Loading, Inner Join
```
*CREATE*
CREATE TABLE table (a INT PRIMARY KEY, b VARCHAR, c VARCHAR, d VARCHAR, f INT, g VARCHAR);


*INSERT*
INSERT INTO table VALUES (number, 'char', 'char', 'char', int num, 'char');

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


*DELETE*
DELETE FROM table WHERE a > '';
DELETE FROM table WHERE a < '';


DELETE FROM table WHERE a = '' OR b = '';
DELETE FROM table WHERE a = '' AND b = '';


DELETE FROM table WHERE (a = '' AND b = '') OR c <= '';
DELETE FROM table WHERE a > '' AND (b = '' OR c = '');


Not supported
DROP/ALTER TABLE
DROP TABLE table;
ALTER TABLE table DROP [Column] <col>;
ALTER TABLE table ADD [Column] <col> int|VARCHAR(n);
ALTER TABLE table RENAME TO <new name>;
ALTER TABLE table RENAME COLUMN <old> TO <new>;

Not supported
SELECT * FROM table ORDER BY <col>; --ORDER BY uses ASC by default. works on VARCHAR too
SELECT * FROM table ORDER BY <col> DESC; --ASC/DESC work without ORDER BY, but with ORDER BY is better.
SELECT * FROM table ORDER BY <col> ASC;  

Not supported
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

Not supported
Aggregates
SELECT COUNT(*) FROM table;
SELECT COUNT(*) FROM table WHERE <expr>;
SELECT SUM(col) FROM table;
SELECT AVG(col) FROM table;
SELECT MIN(col) FROM table;
SELECT MAX(col) FROM table;


Not supported
*JOIN*

-- basic inner join
SELECT * FROM tb1 JOIN tb2 ON tb1.tb2_id = tb2.id;

-- WHERE, ORDER BY, LIMIT, and aggregates all work on the joined result
SELECT * FROM tb1 JOIN tb2 ON tb1.tb2_id = tb2.id WHERE tb2.col1 = ''; 
SELECT * FROM tb1 JOIN tb2 ON tb1.tb2_id = tb2.id ORDER BY salary DESC LIMIT 1;
SELECT SUM(salary) FROM tb1 JOIN tb2 ON tb1.tb2_id = tb2.id WHERE tb2.col1 = 123;

-- unqualified column names work too, as long as they're not ambiguous
SELECT * FROM tb1 JOIN tb2 ON tb1.tb2_id = tb2.id WHERE col2 > 80000;




gcc -Wall -Wextra db.c -o db

\q - exit,\? - for help

./db db.sql
./db sql

Not supported
Batch Execution / File Loading
./db sql < init.sql
```





Not Implemented - Buffer Pool Manager, WAL/Recovery, Catalog, LRU-K replacer, Disk Scheduler, Disk Manager, query optimizer, executor, No free-page list, database wide transactions, Joins, foreign keys, USE statement


