-- 1 init environment
CREATE TABLE interval_sales
( prod_id NUMBER(6)
    , cust_id NUMBER
    , time_id DATE
    , channel_id CHAR(1)
    , promo_id NUMBER(6)
    , quantity_sold NUMBER(3)
    , amount_sold NUMBER(10,2)
)
PARTITION BY RANGE (time_id)
INTERVAL('1 MONTH')
( PARTITION p0 VALUES LESS THAN (TO_DATE('1-1-2008', 'DD-MM-YYYY')),
  PARTITION p1 VALUES LESS THAN (TO_DATE('6-5-2008', 'DD-MM-YYYY'))
);
alter table interval_sales split partition p0 at (to_date('2007-02-10', 'YYYY-MM-DD')) into (partition p0_1, partition p0_2);
select * from interval_sales order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales') order by relname;
insert into interval_sales values(1, 1, to_date('20-2-2009', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('05-2-2009', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('08-2-2009', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('05-4-2009', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('05-8-2009', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('04-8-2009', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('04-9-2008', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('04-11-2018', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('04-01-2019', 'DD-MM-YYYY'), 'a', 1, 1, 1);
insert into interval_sales values(1, 1, to_date('04-02-2019', 'DD-MM-YYYY'), 'a', 1, 1, 1);
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2. cases
-- 2.1 split a interval partition, should be successful. and sys_p2 should be changed to range partition
select * from interval_sales partition(sys_p1) order by time_id;
alter table interval_sales split partition sys_p1 at (to_date('2009-02-10', 'YYYY-MM-DD')) into (partition sys_p1_1, partition sys_p1_2);
select * from interval_sales partition(sys_p1_1) order by time_id;
select * from interval_sales partition(sys_p1_2) order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2.2 split a range partition. (this partition has been changed to range partition in previous step)
select * from interval_sales partition(sys_p2) order by time_id;
alter table interval_sales split partition sys_p2 at (to_date('2009-01-10', 'YYYY-MM-DD')) into (partition sys_p2_1, partition sys_p2_2);
select * from interval_sales partition(sys_p2_1) order by time_id;
select * from interval_sales partition(sys_p2_2) order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2.3 split interval partition sys_p3, successful, and no other partitions should be changed.
select * from interval_sales partition(sys_p3) order by time_id;
alter table interval_sales split partition sys_p3 at (to_date('2009-04-5', 'YYYY-MM-DD')) into (partition sys_p3_1, partition sys_p3_2);
select * from interval_sales partition(sys_p3_1) order by time_id;
select * from interval_sales partition(sys_p3_2) order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2.4 split interval partition sys_p4 at its lower bound, should fail.
select * from interval_sales partition(sys_p4) order by time_id;
alter table interval_sales split partition sys_p4 at (to_date('2009-07-6', 'YYYY-MM-DD')) into (partition sys_p4_1, partition sys_p4_2);
select * from interval_sales partition(sys_p4) order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2.5 split interval partition sys_p4 at its upper bound, should fail.
select * from interval_sales partition(sys_p4) order by time_id;
alter table interval_sales split partition sys_p4 at (to_date('2009-08-06', 'YYYY-MM-DD')) into (partition sys_p4_1, partition sys_p4_2);
select * from interval_sales partition(sys_p4) order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2.6 split sys_P4 at a valid point, should be successful, and no other partitions changed.
select * from interval_sales partition(sys_p4) order by time_id;
alter table interval_sales split partition sys_p4 at (to_date('2009-07-10', 'YYYY-MM-DD')) into (partition sys_p4_1, partition sys_p4_2);
select * from interval_sales partition(sys_p4_1) order by time_id;
select * from interval_sales partition(sys_p4_2) order by time_id;
select relname, parttype, rangenum, intervalnum, partstrategy, interval, boundaries from pg_partition
where parentid = (select oid from pg_class where relname = 'interval_sales')  order by relname;

-- 2.7.0 split p1 using partition def list, no split point given, first partition invalid, equal to previous partition p0_2's boundaries.
alter table interval_sales split partition p1 into
    (partition p1_1 values less than (to_date('2008-01-01', 'YYYY-MM-DD')),
    partition p1_2 values less than (to_date('2008-03-06', 'YYYY-MM-DD')),
    partition p1_3 values less than (to_date('2008-04-06', 'YYYY-MM-DD')),
    partition p1_4 values less than (to_date('2008-05-06', 'YYYY-MM-DD')));

-- 2.7.1 split p1 using partition def list, no split point given, first partition invalid, less than previous partition p0_2's boundaries.
alter table interval_sales split partition p1 into
    (partition p1_1 values less than (to_date('2007-01-01', 'YYYY-MM-DD')),
    partition p1_2 values less than (to_date('2008-03-06', 'YYYY-MM-DD')),
    partition p1_3 values less than (to_date('2008-04-06', 'YYYY-MM-DD')),
    partition p1_4 values less than (to_date('2008-05-06', 'YYYY-MM-DD')));
-- 2.7.2 split p1 using partition def list, no split point given, first partition valid, greater than previous partition p0_2's boundaries.
alter table interval_sales split partition p1 into
    (partition p1_1 values less than (to_date('2008-01-03', 'YYYY-MM-DD')),
    partition p1_2 values less than (to_date('2008-03-06', 'YYYY-MM-DD')),
    partition p1_3 values less than (to_date('2008-04-06', 'YYYY-MM-DD')),
    partition p1_4 values less than (to_date('2008-05-06', 'YYYY-MM-DD')));
select * from interval_sales partition(p1_1)order by time_id;
select * from interval_sales partition(p1_2)order by time_id;
select * from interval_sales partition(p1_3)order by time_id;
select * from interval_sales partition(p1_4)order by time_id;


-- 2.8.1 split sys_p6 using partition def list, no split point given, first partition invalid, equal to self's lower boundaries.
alter table interval_sales split partition sys_p6 into
    (partition sys_p6_1 values less than (to_date('2018-10-6', 'YYYY-MM-DD')),
    partition sys_p6_2 values less than (to_date('2018-10-29', 'YYYY-MM-DD')),
    partition sys_p6_3 values less than (to_date('2018-11-01', 'YYYY-MM-DD')),
    partition sys_p6_4 values less than (to_date('2018-11-06', 'YYYY-MM-DD')));

-- 2.8.2 split sys_p6 using partition def list, no split point given, first partition invalid, less than self's lower boundaries.
alter table interval_sales split partition sys_p6 into
    (partition sys_p6_1 values less than (to_date('2018-10-5', 'YYYY-MM-DD')),
    partition sys_p6_2 values less than (to_date('2018-10-29', 'YYYY-MM-DD')),
    partition sys_p6_3 values less than (to_date('2018-11-01', 'YYYY-MM-DD')),
    partition sys_p6_4 values less than (to_date('2018-11-06', 'YYYY-MM-DD')));
-- 2.8.3 split sys_p6 using partition def list, no split point given, first partition valid, greater than self's lower boundaries.
alter table interval_sales split partition sys_p6 into
    (partition sys_p6_1 values less than (to_date('2018-10-8', 'YYYY-MM-DD')),
    partition sys_p6_2 values less than (to_date('2018-10-29', 'YYYY-MM-DD')),
    partition sys_p6_3 values less than (to_date('2018-11-01', 'YYYY-MM-DD')),
    partition sys_p6_4 values less than (to_date('2018-11-06', 'YYYY-MM-DD')));
select * from interval_sales partition(sys_p6_1)order by time_id;
select * from interval_sales partition(sys_p6_2)order by time_id;
select * from interval_sales partition(sys_p6_3)order by time_id;
select * from interval_sales partition(sys_p6_4)order by time_id;

-- 3 drop table interval_sales
drop table interval_sales;
