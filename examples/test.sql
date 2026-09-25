CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);
INSERT INTO emp VALUES (1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY');
INSERT INTO emp VALUES (2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA');
INSERT INTO emp VALUES (3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF');
INSERT INTO emp VALUES (4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA');
INSERT INTO emp VALUES (5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF');
INSERT INTO emp VALUES (6, 'Frank', '3030C-3001b', 'Sales', 11000, 'NY');

SELECT * FROM emp;
SELECT * FROM emp WHERE id = 1;
SELECT * FROM emp WHERE city = 'NY' AND salary > 10000;
SELECT * FROM emp WHERE dep = 'HR' OR city = 'LA';
SELECT * FROM emp WHERE (city = 'SF' AND dep = 'HR') OR id < 3;
SELECT * FROM emp WHERE dep = 'HR' AND (city = 'SF' OR city = 'NY');




UPDATE emp SET a = 'SF' WHERE id = 1;
UPDATE emp SET did = 'XXX', city = 'Miami' WHERE id >= 5;
UPDATE emp SET salary = 1 WHERE id <= 5 AND (city = 'LA' OR city = 'SF');
