--
-- bypass parallel test prepare
--
set enable_bitmapscan=off;
set enable_seqscan=off;
set opfusion_debug_mode = 'log';

drop table if exists bypass_paral;
create table bypass_paral(col1 int, col2 int, col3 text);
create index ibypass_paral on bypass_paral(col1,col2);

insert into bypass_paral values (0,0,'test_insert');
insert into bypass_paral values (1,1,'test_insert');
insert into bypass_paral values (null,null,'test_insert');
insert into bypass_paral values (null,1,'test_insert');
insert into bypass_paral values (null,2,'test_insert');
insert into bypass_paral values (1,null,'test_insert');


drop table if exists bypass_paral2;
create table bypass_paral2(col1 int, col2 int, col3 text, col4 varchar(6));
create index ibypass_paral2 on bypass_paral2(col2,col1);

insert into bypass_paral2 values (0,0,null,null);
insert into bypass_paral2 values (1,1,0,0);
insert into bypass_paral2 values (2,null,1,1);
insert into bypass_paral2 values (null,1,1,null);
insert into bypass_paral2 values (null,2,3,4);

