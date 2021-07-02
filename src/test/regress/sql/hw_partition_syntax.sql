--
---- test temporary table
--
--sucess
create table test_syntax_temp_1(a int)
partition by range(a)
(
	partition test_syntax_temp_1_p1 values less than (1),
	partition test_syntax_temp_1_p2 values less than (2),
	partition test_syntax_temp_1_p3 values less than (3)
);
--fail
create temporary table test_syntax_temp_2(a int)
partition by range(a)
(
	partition test_syntax_temp_2_p1 values less than (1),
	partition test_syntax_temp_2_p2 values less than (2)
);
--fail
create global temporary table test_syntax_temp_3(a int)
partition by range(a)
(
	partition p1 values less than (1),
	partition p2 values less than (2)
);
--fail
create local temporary table test_syntax_temp_4(a int)
partition by range(a)
(
	partition test_syntax_temp_4_p1 values less than (1),
	partition test_syntax_temp_4_p2 values less than (2)
);

--
---- test unlogged 
--
--fail
create unlogged table test_syntax_unlogged_1(a int)
partition by range(a)
(
	partition test_syntax_unlogged_1_p1 values less than (1),
	partition test_syntax_unlogged_1_p2 values less than (2)
);

--
----test column constraint and collate
--
--sucess
create table test_syntax_col_con_1(a int not null)
partition by range(a)
(
	partition test_syntax_col_con_1_p1 values less than (1),
	partition test_syntax_col_con_1_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_2(a int constraint con_2 not null)
partition by range(a)
(
	partition test_syntax_col_con_2_p1 values less than (1),
	partition test_syntax_col_con_2_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_3(a int null)
partition by range(a)
(
	partition test_syntax_col_con_3_p1 values less than (1),
	partition test_syntax_col_con_3_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_4(a int constraint con_4 null)
partition by range(a)
(
	partition test_syntax_col_con_4_p1 values less than (1),
	partition test_syntax_col_con_4_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_5(a int default 0)
partition by range(a)
(
	partition test_syntax_col_con_5_p1 values less than (1),
	partition test_syntax_col_con_5_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_6(a int constraint con_6 default 0)
partition by range(a)
(
	partition test_syntax_col_con_6_p1 values less than (1),
	partition test_syntax_col_con_6_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_7(a int check(a > 0))
partition by range(a)
(
	partition test_syntax_col_con_7_p1 values less than (1),
	partition test_syntax_col_con_7_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_8(a int constraint con_8 check(a > 0))
partition by range(a)
(
	partition test_syntax_col_con_8_p1 values less than (1),
	partition test_syntax_col_con_8_p2 values less than (2)
);
--fail
create table test_syntax_col_con_9(a int unique)
partition by range(a)
(
	partition test_syntax_col_con_9_p1 values less than (1),
	partition test_syntax_col_con_9_p2 values less than (2)
);
--fail
create table test_syntax_col_con_10(a int primary key )
partition by range(a)
(
	partition test_syntax_col_con_10_p1 values less than (1),
	partition test_syntax_col_con_10_p2 values less than (2)
);
--fail
create table test_syntax_col_con_11(a int check (a > 0) no inherit)
partition by range(a)
(
	partition test_syntax_col_con_11_p1 values less than (1),
	partition test_syntax_col_con_11_p2 values less than (2)
);
--sucess
create table test_syntax_col_con_12(a int , b text collate "C")
partition by range(a)
(
	partition test_syntax_col_con_12_p1 values less than (1),
	partition test_syntax_col_con_12_p2 values less than (2)
);
drop table test_syntax_temp_1;
drop table test_syntax_col_con_1;
drop table test_syntax_col_con_2;
drop table test_syntax_col_con_3;
drop table test_syntax_col_con_4;
drop table test_syntax_col_con_5;
drop table test_syntax_col_con_6;
drop table test_syntax_col_con_7;
drop table test_syntax_col_con_8;
drop table test_syntax_col_con_9;
drop table test_syntax_col_con_10;
drop table test_syntax_col_con_11;
drop table test_syntax_col_con_12;

--
----test table constraint
--
--
----test table constraint
--
--sucess
create table test_syntax_tab_con_1(a int, check(a > 0))
partition by range(a)
(
	partition test_syntax_tab_con_1_p1 values less than (1),
	partition test_syntax_tab_con_1_p2 values less than (2)
);
--fail
create table test_syntax_tab_con_2(a int, check(a > 0) NO INHERIT)
partition by range(a)
(
	partition test_syntax_tab_con_2_p1 values less than (1),
	partition test_syntax_tab_con_2_p2 values less than (2)
);
--fail
create table test_syntax_tab_con_3(a int, unique(a))
partition by range(a)
(
	partition test_syntax_tab_con_3_p1 values less than (1),
	partition test_syntax_tab_con_3_p2 values less than (2)
);
--fail
create table test_syntax_tab_con_4(a int, primary key (a))
partition by range(a)
(
	partition test_syntax_tab_con_4_p1 values less than (1),
	partition test_syntax_tab_con_4_p2 values less than (2)
);
--clean up
drop table test_syntax_tab_con_1;
drop table test_syntax_tab_con_2;
drop table test_syntax_tab_con_3;
drop table test_syntax_tab_con_4;

--
---- test like clause
--
--fail
create table test_syntax_like(b int);
create table test_syntax_like_1(a int, like test_syntax_like)
partition by range(a)
(
	partition test_syntax_like_p1 values less than (1),
	partition test_syntax_like_p2 values less than (2)
);
--clean up
drop table test_syntax_like;
drop table IF EXISTS test_syntax_like_1;

--
----test inherit clause
--
--fail
create table test_syntax_inherit(a int);
create table test_syntax_inherit_1() inherits (test_syntax_inherit)
partition by range(a)
(
	partition test_syntax_inherit_p1 values less than (1),
	partition test_syntax_inherit_p2 values less than (2)
);
--clean up
drop table test_syntax_inherit;

--
----test with option
--
--fail
create table test_syntax_with_1(a int) without oids
partition by range(a)
(
	partition test_syntax_with_1_p1 values less than (1),
	partition test_syntax_with_1_p2 values less than (2)
);
--fail
create table test_syntax_with_2(a int) with oids
partition by range(a)
(
	partition test_syntax_with_2_p1 values less than (1),
	partition test_syntax_with_2_p2 values less than (2)
);
--sucess
create table test_syntax_with_3(a int) WITH (fillfactor=70)
partition by range(a)
(
	partition test_syntax_with_3_p1 values less than (1),
	partition test_syntax_with_3_p2 values less than (2)
);
--clean up
drop table test_syntax_with_3;

--
----test on commit
--
--fail
create table test_syntax_commit_1(a int) on commit drop
partition by range(a)
(
	partition test_syntax_commit_1_p1 values less than (1),
	partition test_syntax_commit_1_p2 values less than (2)
);
--fail
create table test_syntax_commit_2(a int) on commit delete rows
partition by range(a)
(
	partition test_syntax_commit_2_p1 values less than (1),
	partition test_syntax_commit_2_p2 values less than (2)
);
--fail
create table test_syntax_commit_3(a int) on commit preserve rows	
partition by range(a)
(
	partition test_syntax_commit_3_p1 values less than (1),
	partition test_syntax_commit_3_p2 values less than (2)
);

--
--duplicate partition name
--
--fail
create table test_duplicate_partition_name(a bpchar)
partition by range(a)
(
	partition test_duplicate_partition_name_p1 values less than ('H'),
	partition test_duplicate_partition_name_p1 values less than ('Z')
);

--
--undefined partitionkey
--
--fail
create table test_undefined_partitionkey(a int)
partition by range(b)
(
	partition test_undefined_partitionkey_p1 values less than (2),
	partition test_undefined_partitionkey_p2 values less than (4)
);

--
--duplicate partitionkey
--
--fail
create table test_undefined_partitionkey(a int, b int)
partition by range(a, a)
(
	partition test_undefined_partitionkey_p1 values less than (1, 2),
	partition test_undefined_partitionkey_p2 values less than (2, 1)
);

--
--null for partitionvalue
--
--fail
create table test_null_partition_value(a int, b int)
partition by range(a, b)
(
	partition test_null_partition_value_p1 values less than (1, 2),
	partition test_null_partition_value_p2 values less than (2, null)
);

--
--a correct case
--
--sucess
create table test_syntax_correct(a int)
partition by range(a)
(
	partition test_syntax_correct_p1 values less than (1),
	partition test_syntax_correct_p2 values less than (2),
	partition test_syntax_correct_p3 values less than (3)
);
insert into test_syntax_correct values (1),(2);
select * from test_syntax_correct order by 1;
select relname,parttype,rangenum,intervalnum,partstrategy,partkey,boundaries,interval,transit from pg_partition 
	where parentid = (select oid from pg_class where relname = 'test_syntax_correct')
	order by 1, 2, 3;
drop table test_syntax_correct;

create table test__partition_name_length(a int)
partition by range (a)
(
	partition par_123456789_123456789_123456789_123456789_123456789_123456789  values less than (0),--length = 63
	partition part_123456789_123456789_123456789_123456789_123456789_123456789 values less than (10),--length = 64: truncate to NAMEDATALEN - 1
	partition partition_123456789_123456789_123456789_123456789_123456789_123456789  values less than (20)--length = 69: truncate to NAMEDATALEN - 1
);
select relname from pg_partition 
	where parentid = (select oid from pg_class where relname = 'test__partition_name_length')
	order by relname;
--clean up
drop table test__partition_name_length;
--failed
create table stu_info1(sn int, name name)
partition  by range  (sn)
(
	partition stu_info1_p1 values less than('a' like 'a')
);
drop table if exists stu_info1;
--success
create table stu_info2(sn int, name name)
partition  by range  (sn)
(
	partition stu_info2_p1 values less than(int4pl(1,2))
);
drop table if exists stu_info2;
--failed
create table stu_info3(sn int, name name)
partition  by range  (sn)
(
	partition stu_info3_p1 values less than(to_date('2012-02-01', 'YYYY-MM-DD'))
);
drop table if exists stu_info3;

--failed
create table stu_info4(sn int, name name)
partition  by range  (sn)
(
	partition stu_info4_p1 values less than(10)
);
drop table if exists stu_info4;
--success
create table stu_info5(sn int, name name)
partition  by range  (sn)
(
	partition stu_info5_p1 values less than(10)
);
drop table if exists stu_info5;

--success
create table stu_info6(sn int, name name)
partition  by range  (sn)
(
	partition stu_info6_p1 values less than(10)
);
drop table if exists stu_info6;

--failed
create table stu_info7(sn int, name name)
partition  by range  (sn)
(
	partition stu_info7_p1 values less than(10)
);
drop table if exists stu_info7;



--10--------------------------------------------------------------------
--test the row movement syntax
--row movement default values  is false
create table hw_partition_syntax_rowmovement_table
(
	c1 int ,
	c2 int
)
partition by range (c1)
(
	partition hw_partition_syntax_rowmovement_table_p0 values less than (50),
	partition hw_partition_syntax_rowmovement_table_p1 values less than (100),
	partition hw_partition_syntax_rowmovement_table_p2 values less than (150)
);
select relname, relrowmovement from pg_class where relname = 'hw_partition_syntax_rowmovement_table' order by 1, 2;
-- relrowmovement = false
drop table hw_partition_syntax_rowmovement_table;

--enable the row movement
create table hw_partition_syntax_rowmovement_table
(
	c1 int ,
	c2 int
)
partition by range (c1)
(
	partition hw_partition_syntax_rowmovement_table_p0 values less than (50),
	partition hw_partition_syntax_rowmovement_table_p1 values less than (100),
	partition hw_partition_syntax_rowmovement_table_p2 values less than (150)
)enable row movement;
select relname, relrowmovement from pg_class where relname = 'hw_partition_syntax_rowmovement_table';
-- relrowmovement = true
drop table hw_partition_syntax_rowmovement_table;

--disable the rowmovement
create table hw_partition_syntax_rowmovement_table
(
	c1 int ,
	c2 int
)
partition by range (c1)
(
	partition hw_partition_syntax_rowmovement_table_p0 values less than (50),
	partition hw_partition_syntax_rowmovement_table_p1 values less than (100),
	partition hw_partition_syntax_rowmovement_table_p2 values less than (150)
)disable row movement;
select relname, relrowmovement from pg_class where relname = 'hw_partition_syntax_rowmovement_table';
-- relrowmovement = false
drop table hw_partition_syntax_rowmovement_table;


--test the "movement" keyword ,use the "movement" key word as table name and column name
create table movement
(
	movement int
);
--create table succeed
drop table movement;

----------------------------------------------------------------------
