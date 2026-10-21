select * from this;
select * from THIS where this.klant = 'this' -- this in a comment
;
from this select count(*);
describe this;
summarize This;
select a, b from this t join other o on o.id = t.id;
select * from other o left join this on this.id = o.id;
copy (select * from this) to 'out.parquet';
copy this to 'out.csv';
select "this" from this;
select (select max(x) from this) as m;
select count(*) filter (where this.x > 1) from this;
with x as (select * from this) select * from x;
select 1 as this;
select 1 this;
select * from t this;
with this as (select 1) select * from this;
create table this as select 1;
create or replace temp view this as select 1;
insert into this values (1);
update this set a = 1;
delete from this;
drop table this;
alter table this add column z int;
create table if not exists this (a int);
select * from (select 1) this;
select x
  , y
from other
where z in (select z from this)
  and w = 1 as this;
