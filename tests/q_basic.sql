select * from this;
describe this;
select klant, count(*) as n
from this
group by all
order by klant nulls last;
create table t as select 1;
select 42 as x, NULL as leeg, '' as empty;
