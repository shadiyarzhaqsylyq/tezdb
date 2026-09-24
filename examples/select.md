```
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

SELECT * FROM emp WHERE dep = 'HR' AND (city = 'SF' OR city = 'NY');
(5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF')
(6, 'Frank', '3030C-3001b', 'HR', 11000, 'NY')

```
