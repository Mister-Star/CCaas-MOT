--Dynamic SQL TEST
--CREATE schema and table ,INSERT data
SET CHECK_FUNCTION_BODIES TO ON;
CREATE SCHEMA test_user;
create table test_user.test_table(
    ID       INTEGER,
    NAME     varchar2(20),
    AGE      INTEGER,
    ADDRESS  varchar2(20),
    TELE     varchar2(20)
);
insert into test_user.test_table values(1,'leon',10,'adsf');
insert into test_user.test_table values(2,'mary',20,'zcv','234');
insert into test_user.test_table values(3,'mike',30,'zcv','567');

--SELECT INTO in Dynamic SQL
create or replace FUNCTION sp_testsp1()
RETURNS integer 
AS $$
DECLARE
MYCHAR VARCHAR2(20);
PSV_SQL VARCHAR2(200);
BEGIN
     PSV_SQL := 'select name from test_user.test_table where id = 1;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR;
     raise info 'NAME is %', MYCHAR;
     return 0;
END;
$$LANGUAGE plpgsql;
call sp_testsp1();

create or replace FUNCTION sp_testsp2(MYINTEGER IN INTEGER)
returns INTEGER
AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
    BEGIN
        PSV_SQL := 'select name from test_user.test_table where id > '
        ||MYINTEGER||';';
        EXECUTE IMMEDIATE PSV_SQL into MYCHAR;
--        EXCEPTION
--        WHEN NO_DATA_FOUND THEN
--        raise info 'EXCEPTION is NO_DATA_FOUND'; 
--        RETURN 0;                                                                                                                                                       
--        WHEN TOO_MANY_ROWS THEN
--        raise info 'EXCEPTION is TOO_MANY_ROWS';
--        RETURN 0;
    END ;
    raise info 'name is %',MYCHAR;
    RETURN 0;
END;
$$LANGUAGE plpgsql;
select sp_testsp2(1000);
select sp_testsp2(2);
select sp_testsp2(0);

--USING IN
create or replace FUNCTION sp_testsp3()
returns INTEGER
AS $$
DECLARE
  MYINTEGER INTEGER ;
  MYCHAR   VARCHAR2(20);
  PSV_SQL   VARCHAR2(200);
BEGIN
  MYINTEGER := 1;
  PSV_SQL := 'select name from test_user.test_table where id = :a;';
  EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN MYINTEGER;
  raise info 'NAME is %', MYCHAR;
  return 0;
END;
$$LANGUAGE plpgsql;

call sp_testsp3();

--USING INOUT
create or replace FUNCTION sp_testsp4()
returns INTEGER
AS $$
DECLARE
MYCHAR    VARCHAR2(20);
PV_TELE    VARCHAR2(20); 
BEGIN
  MYCHAR := 'MMM'; 
  EXECUTE IMMEDIATE 'update test_user.test_table set tele = :a  where id =1;' USING IN MYCHAR;    
  select tele into PV_TELE from test_user.test_table  where id =1;   
  raise info 'TELE IS %',PV_TELE;
  RETURN 0;
END;
$$LANGUAGE plpgsql;
call sp_testsp4();

--USING IN
create or replace function sp_testsp5(MYCHAR IN VARCHAR2(20))
returns INTEGER
AS $$
DECLARE
PV_TELE VARCHAR2(20); 
BEGIN
  EXECUTE IMMEDIATE 'update test_user.test_table set tele = :a where id =1;' USING IN MYCHAR;
  select tele into PV_TELE from test_user.test_table  where id =1;   
  raise info 'TELE IS %',PV_TELE;
  return 0;
END;
$$LANGUAGE plpgsql;

select sp_testsp5('MMM');

--USING INOUT
create or replace FUNCTION sp_testsp6(MYCHAR INOUT VARCHAR2(20))
returns VARCHAR2
AS $$
BEGIN
  raise notice 'MYCHAR is %', MYCHAR;
  MYCHAR := 'sp_testsp is called';
END;
$$LANGUAGE plpgsql;

create or replace FUNCTION sp_tempsp6()
returns INTEGER
AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
  MYCHAR :=  'THIS IS TEST';
  PSV_SQL := 'call  sp_testsp6(:a)';
  EXECUTE IMMEDIATE PSV_SQL USING IN OUT MYCHAR;                       
  raise info 'MYCHAR is %', MYCHAR;
  RETURN 0;
END;
$$LANGUAGE plpgsql;

call sp_tempsp6();

--USING IN and OUT
create or replace FUNCTION sp_testsp7
(
 MYINTEGER IN INTEGER ,
 MYCHAR   OUT VARCHAR2(200)
)
returns VARCHAR2(200)
AS $$
DECLARE
BEGIN
     MYCHAR := 'sp_testsp is called';
     raise info 'MYINTEGER is %', MYINTEGER;  
  RETURN ;
END;
$$LANGUAGE plpgsql;

create or replace FUNCTION sp_tempsp7()
returns INTEGER
AS $$
DECLARE
  MYINTEGER INTEGER ;
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
  MYINTEGER :=  1;
  PSV_SQL := 'call  sp_testsp7(:a,:b);';
  EXECUTE IMMEDIATE PSV_SQL USING IN MYINTEGER, OUT MYCHAR;
  raise info 'MYCHAR is %', MYCHAR;
  RETURN 0;
END;
$$LANGUAGE plpgsql;

call sp_tempsp7();

----USING IN,COMMAND-STRING is expr
create or replace FUNCTION sp_testsp8(RETURNCODE OUT INTEGER)
returns integer AS $$
DECLARE
  MYCHAR  VARCHAR2(20);
  PSV_SQL VARCHAR2(200);
BEGIN
     PSV_SQL := 'select name from test_user.test_table where id = :a;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN to_number('1')+1;             
     raise notice 'NAME is %', MYCHAR;
END;
$$LANGUAGE plpgsql;

call sp_testsp8(:a);

--USING IN,COMMAND-STRING is constant
create or replace function sp_testsp9( RETURNCODE OUT INTEGER )
returns integer AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
     PSV_SQL := 'select name from test_user.test_table where id = :a';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN 1;        
     raise notice 'NAME is %', MYCHAR;
END; 
$$LANGUAGE plpgsql;

call sp_testsp9(:a);

CREATE OR REPLACE function sp_testsp10
(
    param1    in   INTEGER,
    param2    out  INTEGER,
    param3    in   INTEGER,
    param4    out  INTEGER,
    param5    out  INTEGER
)
returns record as $$
BEGIN
   param2:= param1 + param3;
   param4:= param1 + param2 + param3;
   param5:= param1 + param2 + param3 + param4;
END;
$$ LANGUAGE plpgsql;

create or replace function sp_testsp11() returns void as $$ DECLARE
    input1 INTEGER:=555;
    input2 INTEGER:=111;
    l_statement  VARCHAR2(200);
    l_param2     INTEGER;
    l_param4     INTEGER;
    l_param5     INTEGER;
BEGIN
    l_statement := 'call sp_testsp10(:1,:2,:3,:4,:5)';
    EXECUTE IMMEDIATE l_statement
        USING IN input1, OUT l_param2,IN input2,OUT l_param4,OUT l_param5;
    raise info 'result is:%',l_param2;
    raise info 'result is:%',l_param4;
    raise info 'result is:%',l_param5;
END;
$$ LANGUAGE plpgsql;

call sp_testsp11();

--drop functions,table and schema
drop function sp_testsp1();
drop function sp_testsp2(MYINTEGER IN INTEGER);
drop function sp_testsp3();
drop function sp_testsp4();
drop function sp_testsp5(MYCHAR IN VARCHAR2(20));
drop function sp_testsp6(MYCHAR INOUT VARCHAR2(20));
drop function sp_tempsp6();
drop function sp_testsp7(MYINTEGER IN INTEGER,MYCHAR OUT VARCHAR2(200));
drop function sp_tempsp7();
drop function sp_testsp8(RETURNCODE OUT INTEGER);
drop function sp_testsp9(RETURNCODE OUT INTEGER);
drop function sp_testsp10( param1 in INTEGER, param2 out INTEGER, param3 in INTEGER,param4 out  INTEGER, param5 out  INTEGER);
drop function sp_testsp11();
drop table test_user.test_table CASCADE;
drop schema test_user;

--target into is conflicted with using out
create table mytable(name varchar2(20),id int);
insert into mytable values ('yanyan',1);
create or replace FUNCTION   test1()
returns INTEGER
AS $$
DECLARE
  MYINTEGER INTEGER ;
  myname   VARCHAR2(20);
  MYCHAR   VARCHAR2(20);
  PSV_SQL   VARCHAR2(200);
BEGIN
     MYINTEGER := 1;
     PSV_SQL := 'select name from mytable where id = :a AS :b;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN MYINTEGER,out myname;
     raise info 'NAME is %', MYCHAR;
     return 0;
END;
$$LANGUAGE plpgsql;
DROP TABLE mytable;

--USING IN no denifition
create or replace FUNCTION   test2()
returns INTEGER
AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL   VARCHAR2(200);
BEGIN
     MYINTEGER := 1;
     PSV_SQL := 'select name from mytable where id = :a;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN MYINTEGER;        
     raise info 'NAME is %', MYCHAR;
     return 0;
END;
$$LANGUAGE plpgsql;

--USING OUT no denifition
CREATE OR REPLACE function sp_test_1
(
    param1    in   INTEGER,
    param2    out  INTEGER,
    param3    in   INTEGER
)
AS $$
BEGIN
   param2:= param1 + param3;
END;
$$LANGUAGE plpgsql;

CREATE OR REPLACE function sp_test_2()
returns varchar2(200)
AS $$
DECLARE
    input1 INTEGER:=1;
    input2 INTEGER:=2;
    PSV_SQL  VARCHAR2(200);
BEGIN
    PSV_SQL := 'call sp_test_1(:col_1, :col_2, :col_3)';
    EXECUTE IMMEDIATE l_statement
        USING IN input1, OUT l_param2, IN input2;
    raise info 'result is %', l_param2;
END;
$$LANGUAGE plpgsql;
drop function sp_test_1(param1 in INTEGER,param2 out INTEGER,param3 in INTEGER);
SET CHECK_FUNCTION_BODIES TO OFF;
--Dynamic SQL TEST
--CREATE schema and table ,INSERT data
SET CHECK_FUNCTION_BODIES TO ON;
CREATE SCHEMA test_user;
create table test_user.test_table(
    ID       INTEGER,
    NAME     varchar2(20),
    AGE      INTEGER,
    ADDRESS  varchar2(20),
    TELE     varchar2(20)
);
insert into test_user.test_table values(1,'leon',10,'adsf');
insert into test_user.test_table values(2,'mary',20,'zcv','234');
insert into test_user.test_table values(3,'mike',30,'zcv','567');

--SELECT INTO in Dynamic SQL
create or replace FUNCTION sp_testsp1()
RETURNS integer 
AS $$
DECLARE
MYCHAR VARCHAR2(20);
PSV_SQL VARCHAR2(200);
BEGIN
     PSV_SQL := 'select name from test_user.test_table where id = 1;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR;
     raise info 'NAME is %', MYCHAR;
     return 0;
END;
$$LANGUAGE plpgsql;
call sp_testsp1();

create or replace FUNCTION sp_testsp2(MYINTEGER IN INTEGER)
returns INTEGER
AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
    BEGIN
        PSV_SQL := 'select name from test_user.test_table where id > '
        ||MYINTEGER||';';
        EXECUTE IMMEDIATE PSV_SQL into MYCHAR;
--        EXCEPTION
--        WHEN NO_DATA_FOUND THEN
--        raise info 'EXCEPTION is NO_DATA_FOUND'; 
--        RETURN 0;                                                                                                                                                       
--        WHEN TOO_MANY_ROWS THEN
--        raise info 'EXCEPTION is TOO_MANY_ROWS';
--        RETURN 0;
    END ;
    raise info 'name is %',MYCHAR;
    RETURN 0;
END;
$$LANGUAGE plpgsql;
select sp_testsp2(1000);
select sp_testsp2(2);
select sp_testsp2(0);

--USING IN
create or replace FUNCTION sp_testsp3()
returns INTEGER
AS $$
DECLARE
  MYINTEGER INTEGER ;
  MYCHAR   VARCHAR2(20);
  PSV_SQL   VARCHAR2(200);
BEGIN
  MYINTEGER := 1;
  PSV_SQL := 'select name from test_user.test_table where id = :a;';
  EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN MYINTEGER;
  raise info 'NAME is %', MYCHAR;
  return 0;
END;
$$LANGUAGE plpgsql;

call sp_testsp3();

--USING INOUT
create or replace FUNCTION sp_testsp4()
returns INTEGER
AS $$
DECLARE
MYCHAR    VARCHAR2(20);
PV_TELE    VARCHAR2(20); 
BEGIN
  MYCHAR := 'MMM'; 
  EXECUTE IMMEDIATE 'update test_user.test_table set tele = :a  where id =1;' USING IN MYCHAR;    
  select tele into PV_TELE from test_user.test_table  where id =1;   
  raise info 'TELE IS %',PV_TELE;
  RETURN 0;
END;
$$LANGUAGE plpgsql;
call sp_testsp4();

--USING IN
create or replace function sp_testsp5(MYCHAR IN VARCHAR2(20))
returns INTEGER
AS $$
DECLARE
PV_TELE VARCHAR2(20); 
BEGIN
  EXECUTE IMMEDIATE 'update test_user.test_table set tele = :a where id =1;' USING IN MYCHAR;
  select tele into PV_TELE from test_user.test_table  where id =1;   
  raise info 'TELE IS %',PV_TELE;
  return 0;
END;
$$LANGUAGE plpgsql;

select sp_testsp5('MMM');

--USING INOUT
create or replace FUNCTION sp_testsp6(MYCHAR INOUT VARCHAR2(20))
returns VARCHAR2
AS $$
BEGIN
  raise notice 'MYCHAR is %', MYCHAR;
  MYCHAR := 'sp_testsp is called';
END;
$$LANGUAGE plpgsql;

create or replace FUNCTION sp_tempsp6()
returns INTEGER
AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
  MYCHAR :=  'THIS IS TEST';
  PSV_SQL := 'call  sp_testsp6(:a)';
  EXECUTE IMMEDIATE PSV_SQL USING IN OUT MYCHAR;                       
  raise info 'MYCHAR is %', MYCHAR;
  RETURN 0;
END;
$$LANGUAGE plpgsql;

call sp_tempsp6();

--USING IN and OUT
create or replace FUNCTION sp_testsp7
(
 MYINTEGER IN INTEGER ,
 MYCHAR   OUT VARCHAR2(200)
)
returns VARCHAR2(200)
AS $$
DECLARE
BEGIN
     MYCHAR := 'sp_testsp is called';
     raise info 'MYINTEGER is %', MYINTEGER;  
  RETURN ;
END;
$$LANGUAGE plpgsql;

create or replace FUNCTION sp_tempsp7()
returns INTEGER
AS $$
DECLARE
  MYINTEGER INTEGER ;
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
  MYINTEGER :=  1;
  PSV_SQL := 'call  sp_testsp7(:a,:b);';
  EXECUTE IMMEDIATE PSV_SQL USING IN MYINTEGER, OUT MYCHAR;
  raise info 'MYCHAR is %', MYCHAR;
  RETURN 0;
END;
$$LANGUAGE plpgsql;

call sp_tempsp7();

----USING IN,COMMAND-STRING is expr
create or replace FUNCTION sp_testsp8(RETURNCODE OUT INTEGER)
returns integer AS $$
DECLARE
  MYCHAR  VARCHAR2(20);
  PSV_SQL VARCHAR2(200);
BEGIN
     PSV_SQL := 'select name from test_user.test_table where id = :a;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN to_number('1')+1;             
     raise notice 'NAME is %', MYCHAR;
END;
$$LANGUAGE plpgsql;

call sp_testsp8(:a);

--USING IN,COMMAND-STRING is constant
create or replace function sp_testsp9( RETURNCODE OUT INTEGER )
returns integer AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL  VARCHAR2(200);
BEGIN
     PSV_SQL := 'select name from test_user.test_table where id = :a';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN 1;        
     raise notice 'NAME is %', MYCHAR;
END; 
$$LANGUAGE plpgsql;

call sp_testsp9(:a);

CREATE OR REPLACE function sp_testsp10
(
    param1    in   INTEGER,
    param2    out  INTEGER,
    param3    in   INTEGER,
    param4    out  INTEGER,
    param5    out  INTEGER
)
returns record as $$
BEGIN
   param2:= param1 + param3;
   param4:= param1 + param2 + param3;
   param5:= param1 + param2 + param3 + param4;
END;
$$ LANGUAGE plpgsql;

create or replace function sp_testsp11() returns void as $$ DECLARE
    input1 INTEGER:=555;
    input2 INTEGER:=111;
    l_statement  VARCHAR2(200);
    l_param2     INTEGER;
    l_param4     INTEGER;
    l_param5     INTEGER;
BEGIN
    l_statement := 'call sp_testsp10(:1,:2,:3,:4,:5)';
    EXECUTE IMMEDIATE l_statement
        USING IN input1, OUT l_param2,IN input2,OUT l_param4,OUT l_param5;
    raise info 'result is:%',l_param2;
    raise info 'result is:%',l_param4;
    raise info 'result is:%',l_param5;
END;
$$ LANGUAGE plpgsql;

call sp_testsp11();

--drop functions,table and schema
drop function sp_testsp1();
drop function sp_testsp2(MYINTEGER IN INTEGER);
drop function sp_testsp3();
drop function sp_testsp4();
drop function sp_testsp5(MYCHAR IN VARCHAR2(20));
drop function sp_testsp6(MYCHAR INOUT VARCHAR2(20));
drop function sp_tempsp6();
drop function sp_testsp7(MYINTEGER IN INTEGER,MYCHAR OUT VARCHAR2(200));
drop function sp_tempsp7();
drop function sp_testsp8(RETURNCODE OUT INTEGER);
drop function sp_testsp9(RETURNCODE OUT INTEGER);
drop function sp_testsp10( param1 in INTEGER, param2 out INTEGER, param3 in INTEGER,param4 out  INTEGER, param5 out  INTEGER);
drop function sp_testsp11();
drop table test_user.test_table CASCADE;
drop schema test_user;

--target into is conflicted with using out
create table mytable(name varchar2(20),id int);
insert into mytable values ('yanyan',1);
create or replace FUNCTION   test1()
returns INTEGER
AS $$
DECLARE
  MYINTEGER INTEGER ;
  myname   VARCHAR2(20);
  MYCHAR   VARCHAR2(20);
  PSV_SQL   VARCHAR2(200);
BEGIN
     MYINTEGER := 1;
     PSV_SQL := 'select name from mytable where id = :a AS :b;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN MYINTEGER,out myname;
     raise info 'NAME is %', MYCHAR;
     return 0;
END;
$$LANGUAGE plpgsql;
DROP TABLE mytable;

--USING IN no denifition
create or replace FUNCTION   test2()
returns INTEGER
AS $$
DECLARE
  MYCHAR   VARCHAR2(20);
  PSV_SQL   VARCHAR2(200);
BEGIN
     MYINTEGER := 1;
     PSV_SQL := 'select name from mytable where id = :a;';
     EXECUTE IMMEDIATE PSV_SQL into MYCHAR USING IN MYINTEGER;        
     raise info 'NAME is %', MYCHAR;
     return 0;
END;
$$LANGUAGE plpgsql;

--USING OUT no denifition
CREATE OR REPLACE function sp_test_1
(
    param1    in   INTEGER,
    param2    out  INTEGER,
    param3    in   INTEGER
)
AS $$
BEGIN
   param2:= param1 + param3;
END;
$$LANGUAGE plpgsql;

CREATE OR REPLACE function sp_test_2()
returns varchar2(200)
AS $$
DECLARE
    input1 INTEGER:=1;
    input2 INTEGER:=2;
    PSV_SQL  VARCHAR2(200);
BEGIN
    PSV_SQL := 'call sp_test_1(:col_1, :col_2, :col_3)';
    EXECUTE IMMEDIATE l_statement
        USING IN input1, OUT l_param2, IN input2;
    raise info 'result is %', l_param2;
END;
$$LANGUAGE plpgsql;
drop function sp_test_1(param1 in INTEGER,param2 out INTEGER,param3 in INTEGER);
SET CHECK_FUNCTION_BODIES TO OFF;


--test placehoder
CREATE PROCEDURE calc_stats (
  w NUMBER,
  x NUMBER,
  y NUMBER,
  z NUMBER )
IS
BEGIN
END;
/
DECLARE
  a NUMBER := 4;
  b NUMBER := 7;
  c NUMBER := 8;
  plsql_block VARCHAR2(100);
BEGIN
  plsql_block := 'call calc_stats(:1, $1, :a, :1);';
  EXECUTE IMMEDIATE plsql_block USING a,b,c; 
END;
/

DECLARE
  a NUMBER := 4;
  b NUMBER := 7;
  c NUMBER := 8;
  plsql_block VARCHAR2(100);
BEGIN
  plsql_block := 'call calc_stats($1, $1, :a, :1);';
  EXECUTE IMMEDIATE plsql_block USING a,b,c; 
END;
/

DECLARE
  a NUMBER := 4;
  b NUMBER := 7;
  c NUMBER := 8;
  plsql_block VARCHAR2(100);
BEGIN
  plsql_block := 'call calc_stats(:1, :1, :a, :1);';
  EXECUTE IMMEDIATE plsql_block USING a,b; 
END;
/

DECLARE
  a NUMBER := 4;
  b NUMBER := 7;
  c NUMBER := 8;
  plsql_block VARCHAR2(100);
BEGIN
  plsql_block := 'call calc_stats(:1, :1, :a, :b);';
  EXECUTE IMMEDIATE plsql_block USING a,b; 
END;
/


DECLARE
  a NUMBER := 4;
  b NUMBER := 7;
  c NUMBER := 8;
  plsql_block VARCHAR2(100);
BEGIN
  plsql_block := 'call calc_stats($1, $1, $2, $1);';
  EXECUTE IMMEDIATE plsql_block USING a,b; 
END;
/
drop procedure calc_stats;