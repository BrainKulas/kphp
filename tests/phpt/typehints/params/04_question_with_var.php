@kphp_should_fail
/Cannot parse type hint/
<?php
function foo(? $var) {
}

foo(null);
