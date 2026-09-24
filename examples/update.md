```

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


```
