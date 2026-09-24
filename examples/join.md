```

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

```






```
