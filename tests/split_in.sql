-- leading comment, not a statement
select 1;   select 2
;
/* block ; comment */ select 'a;b' as "x;y";

select $$ dollar ; body $$, $tag$ also ; here $tag$, $1;
.tables
  .mode csv
select
  x  -- trailing ; comment
from t
;;
-- only a comment;
select last_one_without_semicolon
-- tail comment
